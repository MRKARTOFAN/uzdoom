/*
** hw_walls.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "p_local.h"
#include "p_lnspec.h"
#include "a_dynlight.h"
#include "a_sharedglobal.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "p_maputl.h"
#include "doomdata.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "texturemanager.h"
#include "hw_dynlightdata.h"
#include "hw_material.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hw_lighting.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "hw_skydome.h"
#include "hw_walldispatcher.h"
#include "m_round.h"
#include "hw_vertexbuilder.h"
#include <array>
#include <set>
#include <unordered_map>

struct HWPreparedWall
{
	std::array<FFlatVertex, 4> Vertices;
	FVector3 Normal;
	IVertexBuffer* Buffer = nullptr;
	unsigned Index = 0;
	unsigned LightingSlot = ~0u;
	unsigned OwnedLightingSlot = ~0u;
	uint64_t OwnedLightingVersion = 0;
	uint64_t OwnedLightingContext = 0;
};

CVAR(Bool, gl_wallcache, false, 0)
CVAR(Bool, gl_wallcache_verify, false, 0)
CVAR(Int, vk_retainedworld, 0, 0)
CVAR(Bool, vk_retainedworld_verify, false, 0)
CVAR(Bool, vk_retainedworld_directkey, false, 0)
CVAR(Bool, vk_sectorlayout, false, 0)
CVAR(Bool, vk_worldruns, false, 0)
CVAR(Bool, vk_drawownership, false, 0)
CVAR(Bool, vk_ownedstate, false, 0)
CVAR(Bool, vk_snapshot_fast, false, 0)
CVAR(Bool, vk_gpu_driven_persistent, false, 0)
CVAR(Bool, vk_gpu_driven_staticmaterial, false, 0)
EXTERN_CVAR(Bool, vk_gpu_driven_resident)
EXTERN_CVAR(Bool, vk_gpu_driven_candidate)
EXTERN_CVAR(Bool, vk_gpu_driven_geometry)
EXTERN_CVAR(Bool, vk_gpu_driven_indexed)
EXTERN_CVAR(Bool, vk_gpu_driven_bindless)
EXTERN_CVAR(Bool, vk_gpu_driven_draw)
EXTERN_CVAR(Bool, vk_gpu_driven_bake)
EXTERN_CVAR(Bool, vk_gpu_driven_debug)
EXTERN_CVAR(Int, r_distance_cull_type)
EXTERN_CVAR(Bool, vk_sharedappearance)
EXTERN_CVAR(Bool, gl_texture)
EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Bool, gl_seamless)
EXTERN_CVAR(Int, r_fakecontrast)

static void CapturePreparedWall(HWWallDispatcher* di, HWWall* wall);


void SetGlowPlanes(FRenderState &state, const secplane_t& top, const secplane_t& bottom)
{
	auto& tn = top.Normal();
	auto& bn = bottom.Normal();
	FVector4 tp = { (float)tn.X, (float)tn.Y, (float)top.negiC, (float)top.fD() };
	FVector4 bp = { (float)bn.X, (float)bn.Y, (float)bottom.negiC, (float)bottom.fD() };
	state.SetGlowPlanes(tp, bp);
}

void SetGradientPlanes(FRenderState& state, const secplane_t& top, const secplane_t& bottom)
{
	auto& tn = top.Normal();
	auto& bn = bottom.Normal();
	FVector4 tp = { (float)tn.X, (float)tn.Y, (float)top.negiC, (float)top.fD() };
	FVector4 bp = { (float)bn.X, (float)bn.Y, (float)bottom.negiC, (float)bottom.fD() };
	state.SetGradientPlanes(tp, bp);
}

void SetSplitPlanes(FRenderState& state, const secplane_t& top, const secplane_t& bottom)
{
	auto& tn = top.Normal();
	auto& bn = bottom.Normal();
	FVector4 tp = { (float)tn.X, (float)tn.Y, (float)top.negiC, (float)top.fD() };
	FVector4 bp = { (float)bn.X, (float)bn.Y, (float)bottom.negiC, (float)bottom.fD() };
	state.SetSplitPlanes(tp, bp);
}


//==========================================================================
//
// General purpose wall rendering function
// everything goes through here
//
//==========================================================================

static void CaptureGpuOwnerStaticMaterial(HWWall* wall, FRenderState& state);

void HWWall::RenderWall(FRenderState &state, int textured, uint32_t gpuSlot)
{
	bool ditherT = false;
	if (seg->sidedef != nullptr)
	{
		ditherT = (type == RENDERWALL_BOTTOM) && (seg->sidedef->Flags & WALLF_DITHERTRANS_BOTTOM);
		ditherT |= (type == RENDERWALL_TOP) && (seg->sidedef->Flags & WALLF_DITHERTRANS_TOP);
		ditherT = ditherT || (seg->sidedef->Flags & WALLF_DITHERTRANS_MID);
	}
	if (ditherT)
	{
		state.SetEffect(EFF_DITHERTRANS);
	}
	state.SetLightIndex(dynlightindex);
	if (vk_gpu_driven_staticmaterial && GpuOwnerSlot >= 0) CaptureGpuOwnerStaticMaterial(this, state);
	const bool gpuDrawn = gpuSlot != UINT32_MAX && state.DrawGpuDrivenWall(gpuSlot, GpuBakeRow);
	if (!gpuDrawn && gpuSlot != UINT32_MAX && !vertcount) MakeVertices(false);
	bool baked = gpuDrawn;
	assert(gpuDrawn || vertcount > 0);
	if (!baked && vk_gpu_driven_draw && vertcount == 4 && CanCacheVertices(true))
	{
		FFlatVertex current[4];
		FFlatVertex* cursor = current;
		CreateVertices(cursor, false);
		uint32_t iboOffset = 0, indexCount = 0;
		if (GPUBakeLookupWall(seg, type, current, &iboOffset, &indexCount))
		{
			baked = state.DrawBakedPrimitive(iboOffset, indexCount);
			if (baked && vk_gpu_driven_debug)
			{
				static std::set<uint64_t> logged;
				const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(seg)) << 8) ^ uint64_t(type & 0xff);
				if (logged.insert(key).second)
					Printf("G1DRAW seg=%p type=%d off=%u cnt=%u\n", (void*)seg, type, iboOffset, indexCount);
			}
		}
	}
	if (!baked)
	{
		if (Prepared && (textured & RWF_BATCH) && vk_retainedworld == 2 && vertcount == 4)
			state.DrawRetainedQuad(vertindex);
		else if ((textured & RWF_BATCH) && vertcount == 4)
			state.DrawOpaqueQuad(vertindex);
		else
			state.Draw(DT_TriangleFan, vertindex, vertcount);
	}
	vertexcount += gpuDrawn ? 4 : vertcount;
	if (ditherT)
	{
		state.SetEffect(EFF_NONE);
		switch(type) // reset this every frame
		{
		case RENDERWALL_TOP:
			seg->sidedef->Flags &= ~WALLF_DITHERTRANS_TOP;
			break;
		case RENDERWALL_BOTTOM:
			seg->sidedef->Flags &= ~WALLF_DITHERTRANS_BOTTOM;
			break;
		default:
			if (seg->sidedef->dithertranscount-- <= 0) seg->sidedef->Flags &= ~WALLF_DITHERTRANS_MID;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

void HWWall::RenderFogBoundary(HWWallDispatcher*di, FRenderState &state)
{
	if (gl_fogmode && !di->isFullbrightScene())
	{
		int rel = rellight + getExtraLight();
		state.EnableDrawBufferAttachments(false);
		SetFog(state, di->Level, di->lightmode, lightlevel, rel, false, &Colormap, false);
		state.SetEffect(EFF_FOGBOUNDARY);
		state.AlphaFunc(Alpha_GEqual, 0.f);
		state.SetDepthBias(-1, -128);
		RenderWall(state, HWWall::RWF_BLANK);
		state.ClearDepthBias();
		state.SetEffect(EFF_NONE);
		state.EnableDrawBufferAttachments(true);
	}
}


//==========================================================================
//
//
//
//==========================================================================
void HWWall::RenderMirrorSurface(HWWallDispatcher*di, FRenderState &state)
{
	if (!TexMan.mirrorTexture.isValid()) return;

	state.SetDepthFunc(DF_LEqual);

	// we use texture coordinates and texture matrix to pass the normal stuff to the shader so that the default vertex buffer format can be used as is.
	state.EnableTextureMatrix(true);

	// Use sphere mapping for this
	state.SetEffect(EFF_SPHEREMAP);
	SetColor(state, di->Level, di->lightmode, lightlevel, 0, di->isFullbrightScene(), Colormap, 0.1f);
	SetFog(state, di->Level, di->lightmode, lightlevel, 0, di->isFullbrightScene(), &Colormap, true);
	state.SetRenderStyle(STYLE_Add);
	state.AlphaFunc(Alpha_Greater, 0);

	auto tex = TexMan.GetGameTexture(TexMan.mirrorTexture, false);
	state.SetMaterial(tex, UF_None, 0, CLAMP_NONE, NO_TRANSLATION, -1, nullptr); // do not upscale the mirror texture.

	flags &= ~HWWall::HWF_GLOW;
	RenderWall(state, HWWall::RWF_BLANK);

	state.EnableTextureMatrix(false);
	state.SetEffect(EFF_NONE);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);

	state.SetDepthFunc(DF_Less);

	// This is drawn in the translucent pass which is done after the decal pass
	// As a result the decals have to be drawn here, right after the wall they are on,
	// because the depth buffer won't get set by translucent items.
	if (di->di && seg->sidedef->AttachedDecals)
	{
		DrawDecalsForMirror(di->di, state, di->di->Decals[1]);
	}
	state.SetRenderStyle(STYLE_Translucent);
}

//==========================================================================
//
//
//
//==========================================================================

static const uint8_t renderwalltotier[] =
{
	side_t::none,
	side_t::top,
	side_t::mid,
	side_t::mid,
	side_t::bottom,
	side_t::none,
	side_t::none,
	side_t::mid,
	side_t::none,
	side_t::mid,
};

#ifdef NPOT_EMULATION
CVAR(Bool, hw_npottest, false, 0)
#endif

void HWWall::RenderTexturedWall(HWWallDispatcher*di, FRenderState &state, int rflags, uint32_t gpuSlot)
{
	int tmode = state.GetTextureMode();
	int rel = rellight + getExtraLight();

	if (flags & HWWall::HWF_GLOW)
	{
		state.EnableGlow(true);
		state.SetGlowParams(topglowcolor, bottomglowcolor);
		SetGlowPlanes(state, frontsector->ceilingplane, frontsector->floorplane);
	}
	state.SetMaterial(texture, UF_Texture, 0, flags & 3, NO_TRANSLATION, -1, nullptr);
#ifdef NPOT_EMULATION
	// Test code, could be reactivated as a compatibility option in the unlikely event that some old vanilla map eve needs it.
	if (hw_npottest)
	{
		int32_t size = RoundHalfEven(texture->GetDisplayHeight());
		int32_t size2;
		for (size2 = 1; size2 < size; size2 += size2) {}
		if (size == size2)
			state.SetNpotEmulation(0.f, 0.f);
		else
		{
			float xOffset = 1.f / texture->GetDisplayWidth();
			state.SetNpotEmulation((1.f * size2) / size, xOffset);
		}
	}
#endif

	if (flags & HWWall::HWF_CLAMPY && (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF) && !texture->isWarped())
	{
		state.SetTextureClamp(true);
	}

	if (type == RENDERWALL_M2SNF)
	{
		SetFog(state, di->Level, di->lightmode, 255, 0, di->isFullbrightScene(), nullptr, false);
	}
	if (type != RENDERWALL_COLOR && seg->sidedef != nullptr)
	{
		auto side = seg->sidedef;
		if (seg->sidedef->Flags & WALLF_EXTCOLOR) // this block incurs a costly cache miss, so only process if needed.
		{

			auto tierndx = renderwalltotier[type];
			auto& tier = side->textures[tierndx];
			PalEntry color1 = side->GetSpecialColor(tierndx, side_t::walltop, frontsector);
			PalEntry color2 = side->GetSpecialColor(tierndx, side_t::wallbottom, frontsector);
			state.SetObjectColor(color1);
			state.SetObjectColor2((color1 != color2) ? color2 : PalEntry(0));
			state.SetAddColor(side->GetAdditiveColor(tierndx, frontsector));
			state.ApplyTextureManipulation(&tier.TextureFx);

			if (color1 != color2)
			{
				// Do gradient setup only if there actually is a gradient.

				state.EnableGradient(true);
				if ((tier.flags & side_t::part::ClampGradient) && backsector)
				{
					if (tierndx == side_t::top)
					{
						SetGradientPlanes(state, frontsector->ceilingplane, backsector->ceilingplane);
					}
					else if (tierndx == side_t::mid)
					{
						SetGradientPlanes(state, backsector->ceilingplane, backsector->floorplane);
					}
					else // side_t::bottom:
					{
						SetGradientPlanes(state, backsector->floorplane, frontsector->floorplane);
					}
				}
				else
				{
					SetGradientPlanes(state, frontsector->ceilingplane, frontsector->floorplane);
				}
			}
		}
	}

	float absalpha = fabsf(alpha);
	if (lightlist == nullptr)
	{
		bool appearanceDone = false;
		if (vk_sharedappearance && !vk_retainedworld && type != RENDERWALL_M2SNF && di->di && !di->Level->HasDynamicLights)
		{
			FSharedAppearanceKey key = {};
			key.LightMode = int(di->lightmode);
			key.LightLevel = lightlevel;
			key.RelativeLight = rel;
			key.LightColor = Colormap.LightColor.d;
			key.FadeColor = Colormap.FadeColor.d;
			key.Desaturation = Colormap.Desaturation;
			key.BlendFactor = Colormap.BlendFactor;
			key.FogDensity = Colormap.FogDensity;
			key.Alpha = absalpha;
			key.Additive = RenderStyle == STYLE_Add;
			key.Fullbright = di->isFullbrightScene();
			key.SoftwareLighting = isSoftwareLighting(di->lightmode);
			key.FogMode = gl_fogmode;
			key.CullType = r_distance_cull_type;
			key.LevelFlags = di->Level->flags;
			key.CullDist = di->Level->culldist;
			key.CullColor = di->Level->cullcolor.d;
			key.OutsideFog = di->Level->info->outsidefog;
			key.BrightFog = di->Level->brightfog;
			key.InSkybox = portalState.inskybox;
			key.Level = di->Level;
			appearanceDone = state.SelectSharedAppearance(key);
			if (!appearanceDone)
			{
				SetFog(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), &Colormap, RenderStyle == STYLE_Add);
				SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, absalpha);
				state.StoreSharedAppearance(key);
				appearanceDone = true;
			}
		}
		if (!appearanceDone)
		{
			if (!Prepared || !vk_retainedworld || type == RENDERWALL_M2SNF ||
				!ApplyRetainedWorldLighting(di->di, state, Prepared->LightingSlot,
					lightlevel, rel, Colormap, absalpha, RenderStyle == STYLE_Add, true))
			{
				if (type != RENDERWALL_M2SNF) SetFog(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), &Colormap, RenderStyle == STYLE_Add);
				SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, absalpha);
			}
		}
		RenderWall(state, rflags, gpuSlot);
	}
	else
	{
		state.EnableSplit(true);

		for (unsigned i = 0; i < lightlist->Size(); i++)
		{
			secplane_t &lowplane = i == (*lightlist).Size() - 1 ? frontsector->floorplane : (*lightlist)[i + 1].plane;
			// this must use the exact same calculation method as HWWall::Process etc.
			float low1 = lowplane.ZatPoint(vertexes[0]);
			float low2 = lowplane.ZatPoint(vertexes[1]);

			if (low1 < ztop[0] || low2 < ztop[1])
			{
				int thisll = (*lightlist)[i].caster != nullptr ? RescaleLightLevel(*(*lightlist)[i].p_lightlevel) : lightlevel;
				FColormap thiscm;
				thiscm.FadeColor = Colormap.FadeColor;
				thiscm.FogDensity = Colormap.FogDensity;
				CopyFrom3DLight(thiscm, &(*lightlist)[i]);
				SetColor(state, di->Level, di->lightmode, thisll, rel, false, thiscm, absalpha);
				if (type != RENDERWALL_M2SNF) SetFog(state, di->Level, di->lightmode, thisll, rel, false, &thiscm, RenderStyle == STYLE_Add);
				SetSplitPlanes(state, (*lightlist)[i].plane, lowplane);
				RenderWall(state, rflags);
			}
			if (low1 <= zbottom[0] && low2 <= zbottom[1]) break;
		}

		state.EnableSplit(false);
	}
	state.SetNpotEmulation(0.f, 0.f);
	state.SetObjectColor(0xffffffff);
	state.SetObjectColor2(0);
	state.SetAddColor(0);
	state.SetTextureMode(tmode);
	state.SetTextureClamp(false);
	state.EnableGlow(false);
	state.EnableGradient(false);
	state.ApplyTextureManipulation(nullptr);
}

//==========================================================================
//
//
//
//==========================================================================

void HWWall::RenderTranslucentWall(HWWallDispatcher*di, FRenderState &state)
{
	state.SetRenderStyle(RenderStyle);
	if (texture)
	{
		if (!texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
		else state.AlphaFunc(Alpha_GEqual, 0.f);
		RenderTexturedWall(di, state, HWWall::RWF_TEXTURED | HWWall::RWF_NOSPLIT);
	}
	else
	{
		state.AlphaFunc(Alpha_GEqual, 0.f);
		SetColor(state, di->Level, di->lightmode, lightlevel, 0, false, Colormap, fabsf(alpha));
		SetFog(state, di->Level, di->lightmode, lightlevel, 0, false, &Colormap, RenderStyle == STYLE_Add);
		state.EnableTexture(false);
		RenderWall(state, HWWall::RWF_NOSPLIT);
		state.EnableTexture(true);
	}
	state.SetRenderStyle(STYLE_Translucent);
}

//==========================================================================
//
//
//
//==========================================================================
void HWWall::DrawWall(HWWallDispatcher*di, FRenderState &state, bool translucent, uint32_t gpuSlot)
{
	if (SceneProfiling) ++GetSceneSurfaceProfile().DrawCalls;
	IVertexBuffer *cachedBuffer = nullptr;
	unsigned originalIndex = vertindex;
	if (screen->BuffersArePersistent())
	{
		if (di->di && di->Level->HasDynamicLights && !di->isFullbrightScene() && texture != nullptr)
		{
			SetupLights(di->di, lightdata);
		}
		if (Prepared && vk_retainedworld)
		{
			if (!Prepared->Buffer)
			{
				Prepared->Buffer = screen->mVertexData->RetainPreparedWallVertices(Prepared->Vertices.data(), Prepared->Index);
			}
			cachedBuffer = Prepared->Buffer;
			if (cachedBuffer)
			{
				vertindex = Prepared->Index;
				vertcount = 4;
			}
		}
		else if ((gl_wallcache || SceneProfiling) && screen->IsVulkan() && CanCacheVertices())
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().CacheEligible;
			if (gl_wallcache)
			{
				if (SceneProfiling) ++GetSceneSurfaceProfile().CacheRequests;
				// [Karto] Compare the complete original vertex output so direct script writes and interpolation cannot bypass invalidation.
				FFlatVertex vertices[4];
				auto ptr = vertices;
				CreateVertices(ptr, false);
				cachedBuffer = screen->mVertexData->CacheWallVertices(seg->sidedef, type, vertices, vertindex, gl_wallcache_verify);
				if (cachedBuffer) vertcount = 4;
			}
		}
		if (!cachedBuffer && gpuSlot == UINT32_MAX) MakeVertices(!!(flags & HWWall::HWF_TRANSLUCENT));
	}

	if (cachedBuffer) state.SetVertexBuffer(cachedBuffer, 0, 0);
	state.SetNormal(Prepared ? Prepared->Normal : glseg.Normal());
	if (!translucent)
	{
		RenderTexturedWall(di, state, HWWall::RWF_TEXTURED | (cachedBuffer ? HWWall::RWF_BATCH : 0), gpuSlot);
	}
	else
	{
		switch (type)
		{
		case RENDERWALL_MIRRORSURFACE:
			RenderMirrorSurface(di, state);
			break;

		case RENDERWALL_FOGBOUNDARY:
			RenderFogBoundary(di, state);
			break;

		default:
			RenderTranslucentWall(di, state);
			break;
		}
	}
	if (cachedBuffer)
	{
		// [Karto] Later draws and portal/decal helpers still use the original stream buffer and its indices.
		state.SetVertexBuffer(screen->mVertexData);
		vertindex = originalIndex;
		vertcount = 0;
	}
}

bool HWWall::CanDrawPersistentWall(unsigned* rejected) const
{
	if (!Prepared || vertcount || !texture || lightlist || lightmap ||
		(type != RENDERWALL_TOP && type != RENDERWALL_M1S && type != RENDERWALL_BOTTOM) ||
		(flags & (HWF_GLOW | HWF_TRANSLUCENT | HWF_SKYHACK)) || RenderStyle != STYLE_Normal || alpha != 1.f ||
		texture->isMasked() || texture->GetTranslucency() ||
		!seg || !seg->sidedef || seg->sidedef->AttachedDecals ||
		(seg->sidedef->Flags & (WALLF_POLYOBJ | WALLF_DITHERTRANS_TOP | WALLF_DITHERTRANS_BOTTOM | WALLF_DITHERTRANS_MID)))
	{
		if (rejected)
		{
			*rejected |=
				(!Prepared ? 1u : 0u) | (vertcount ? 2u : 0u) | (!texture ? 4u : 0u) |
				(lightlist ? 8u : 0u) | (lightmap ? 16u : 0u) |
				((type != RENDERWALL_TOP && type != RENDERWALL_M1S && type != RENDERWALL_BOTTOM) ? 32u : 0u) |
				((flags & (HWF_GLOW | HWF_TRANSLUCENT | HWF_SKYHACK)) ? 64u : 0u) |
				(RenderStyle != STYLE_Normal ? 128u : 0u) | (alpha != 1.f ? 256u : 0u) |
				((texture && texture->isMasked()) ? 512u : 0u) | ((texture && texture->GetTranslucency()) ? 1024u : 0u) |
				((!seg || !seg->sidedef) ? 2048u : 0u) |
				((seg && seg->sidedef && seg->sidedef->AttachedDecals) ? 4096u : 0u) |
				((seg && seg->sidedef && (seg->sidedef->Flags & WALLF_POLYOBJ)) ? 16384u : 0u) |
				((seg && seg->sidedef && (seg->sidedef->Flags & (WALLF_DITHERTRANS_TOP | WALLF_DITHERTRANS_BOTTOM | WALLF_DITHERTRANS_MID))) ? 32768u : 0u);
		}
		return false;
	}
	if (seg->sidedef->Flags & WALLF_EXTCOLOR)
	{
		// [Karto] Map initialization also flags neutral walls; compare live tier inputs so direct script writes still leave the run immediately.
		auto side = seg->sidedef;
		const auto tier = renderwalltotier[type];
		if (!frontsector || side->GetSpecialColor(tier, side_t::walltop, frontsector) != 0xffffffff ||
			side->GetSpecialColor(tier, side_t::wallbottom, frontsector) != 0xffffffff ||
			side->GetAdditiveColor(tier, frontsector) != 0 || side->textures[tier].TextureFx.AddColor.a != 0)
		{
			if (rejected) *rejected |= 8192u;
			return false;
		}
	}
#ifdef NPOT_EMULATION
	if (hw_npottest) return false;
#endif
	return true;
}

bool HWWall::DrawPreparedWall(HWWallDispatcher* di, FRenderState& state, HWPreparedWallRun& run, uint64_t ownerVersion)
{
	auto reject = [](uint64_t& count)
	{
		if (SceneProfiling) ++count;
		return false;
	};
	unsigned rejected = 0;
	// [Karto] Sight-dependent dither flags can change after preparation; ownership cannot suppress their live fallback.
	if (!CanDrawPersistentWall(SceneProfiling ? &rejected : nullptr))
	{
		if (SceneProfiling) SceneMainSurfaceProfile.PreparedDrawRejectedMask |= rejected;
		return reject(SceneMainSurfaceProfile.PreparedDrawRejectedGeometry);
	}
	if (!run.Buffer && !state.CanBeginPreparedWallRun()) return reject(SceneMainSurfaceProfile.PreparedDrawRejectedState);
	if (!Prepared->Buffer)
		Prepared->Buffer = screen->mVertexData->RetainPreparedWallVertices(Prepared->Vertices.data(), Prepared->Index);
	if (!Prepared->Buffer) return reject(SceneMainSurfaceProfile.PreparedDrawRejectedGeometry);
	const int clampMode = flags & 3;
	const bool newMaterial = run.Texture != texture || run.ClampMode != clampMode;
	if (newMaterial) state.SetMaterial(texture, UF_Texture, 0, clampMode, NO_TRANSLATION, -1, nullptr);
	if (!state.HasPreparedWallMaterial()) return reject(SceneMainSurfaceProfile.PreparedDrawRejectedMaterial);
	const bool newBuffer = run.Buffer != Prepared->Buffer;
	if (newBuffer) state.SetVertexBuffer(Prepared->Buffer, 0, 0);
	state.SetNormal(Prepared->Normal);
	const int relativeLight = rellight + getExtraLight();
	const bool ownedState = vk_ownedstate && ownerVersion != 0;
	auto& lightingSlot = ownedState ? Prepared->OwnedLightingSlot : Prepared->LightingSlot;
	if (!ApplyRetainedWorldLighting(di->di, state, lightingSlot,
		lightlevel, relativeLight, Colormap, fabsf(alpha), RenderStyle == STYLE_Add, true,
		ownedState ? Prepared : nullptr, ownerVersion))
	{
		ownerVersion = 0;
		SetFog(state, di->Level, di->lightmode, lightlevel, relativeLight, di->isFullbrightScene(), &Colormap, false);
		SetColor(state, di->Level, di->lightmode, lightlevel, relativeLight, di->isFullbrightScene(), Colormap, fabsf(alpha));
	}
	state.SetLightIndex(dynlightindex);
	state.PrepareRetainedWallRun(newMaterial || newBuffer, ownerVersion != 0);
	if (ownerVersion) state.DrawOwnedRetainedQuad(Prepared->Index, ownerVersion);
	else state.DrawRetainedQuad(Prepared->Index);
	vertexcount += 4;
	if (SceneProfiling)
	{
		++SceneMainSurfaceProfile.DrawCalls;
		++SceneMainSurfaceProfile.PreparedDrawPackets;
		if (newMaterial || newBuffer) ++SceneMainSurfaceProfile.PreparedDrawRuns;
	}
	run.Texture = texture;
	run.Buffer = Prepared->Buffer;
	run.ClampMode = clampMode;
	return true;
}

bool HWWall::CanCacheVertices(bool allowGenerated) const
{
	if (type != RENDERWALL_TOP && type != RENDERWALL_M1S && type != RENDERWALL_BOTTOM) return false;
	if ((vertcount && !allowGenerated) || gl_seamless || (flags & HWF_TRANSLUCENT) || RenderStyle != STYLE_Normal || alpha != 1.0f) return false;
	if (!texture || texture->isMasked() || texture->GetTranslucency() || lightmap) return false;
	if (!seg || !seg->sidedef || !seg->linedef || (seg->sidedef->Flags & WALLF_POLYOBJ) || seg->sidedef->AttachedDecals) return false;
	if (seg->linedef->isVisualPortal() || seg->linedef->GetTransferredPortal()) return false;
	if (frontsector != seg->frontsector || backsector != seg->backsector) return false;
	auto ordinary = [](const sector_t *sector)
	{
		return !sector || (!sector->floorplane.isSlope() && !sector->ceilingplane.isSlope() &&
			!sector->GetHeightSec() && sector->e->XFloor.ffloors.Size() == 0 && sector->e->XFloor.lightlist.Size() == 0);
	};
	return ordinary(frontsector) && ordinary(backsector);
}

bool HWWall::CanGpuDriveWall(bool allowGenerated) const
{
	if (!CanCacheVertices(allowGenerated)) return false;
	if (!texture || texture->GetShaderIndex() != 0 || texture->isHardwareCanvas() || texture->isFullbright()) return false;
	if (RenderStyle != STYLE_Normal || alpha != 1.0f) return false;
	if (!frontsector || !frontsector->Level || frontsector->Level->HasDynamicLights || sysCallbacks.PreBindTexture ||
		(flags & (HWF_GLOW | HWF_TRANSLUCENT | HWF_SKYHACK))) return false;
	if (seg->sidedef->Flags & (WALLF_DITHERTRANS_TOP | WALLF_DITHERTRANS_BOTTOM | WALLF_DITHERTRANS_MID)) return false;
	TArray<FTexture*> layers;
	texture->GetLayers(layers);
	return layers.Size() >= 1 && layers.Size() <= 4;
}

//==========================================================================
//
// Collect lights for shader
//
//==========================================================================

void HWWall::SetupLights(HWDrawInfo*di, FDynLightData &lightdata)
{
	lightdata.Clear();

	if (RenderStyle == STYLE_Add && !di->Level->lightadditivesurfaces) return;	// no lights on additively blended surfaces.

	// check for wall types which cannot have dynamic lights on them (portal types never get here so they don't need to be checked.)
	switch (type)
	{
	case RENDERWALL_FOGBOUNDARY:
	case RENDERWALL_MIRRORSURFACE:
	case RENDERWALL_COLOR:
		return;
	}

	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;


	auto normal = glseg.Normal();
	p.Set(normal, -normal.X * glseg.x1 - normal.Z * glseg.y1);

	if ((seg->sidedef->Flags & WALLF_POLYOBJ) && sub)
	{
		// Polobject segs cannot be checked per sidedef so use the subsector instead.
		for(FDynamicLight * lightsource : sub->section->dlist)
		{
			if (lightsource->IsActive() && !lightsource->DontLightMap())
			{
				iter_dlight++;

				DVector3 posrel = lightsource->PosRelative(seg->frontsector->PortalGroup);
				float x = posrel.X;
				float y = posrel.Y;
				float z = posrel.Z;
				float dist = fabsf(p.DistToPoint(x, z, y));
				float radius = lightsource->GetRadius();
				float scale = 1.0f / ((2.f * radius) - dist);
				FVector3 fn, pos;

				if (radius > 0.f && dist < radius)
				{
					FVector3 nearPt, up, right;

					pos = { x, z, y };
					fn = p.Normal();

					fn.GetRightUp(right, up);

					FVector3 tmpVec = fn * dist;
					nearPt = pos + tmpVec;

					FVector3 t1;
					int outcnt[4]={0,0,0,0};
					texcoord tcs[4];

					// do a quick check whether the light touches this polygon
					for(int i=0;i<4;i++)
					{
						t1 = FVector3(&vtx[i*3]);
						FVector3 nearToVert = t1 - nearPt;
						tcs[i].u = ((nearToVert | right) * scale) + 0.5f;
						tcs[i].v = ((nearToVert | up) * scale) + 0.5f;

						if (tcs[i].u<0) outcnt[0]++;
						if (tcs[i].u>1) outcnt[1]++;
						if (tcs[i].v<0) outcnt[2]++;
						if (tcs[i].v>1) outcnt[3]++;

					}
					if (outcnt[0]!=4 && outcnt[1]!=4 && outcnt[2]!=4 && outcnt[3]!=4)
					{
						draw_dlight += GetLight(lightdata, seg->frontsector->PortalGroup, p, lightsource, true);
					}
				}
			}
		}
	}
	else
	{
		for(FDynamicLight * lightsource : seg->sidedef->dlist)
		{
			if (lightsource->IsActive() && !lightsource->DontLightMap())
			{
				iter_dlight++;

				DVector3 posrel = lightsource->PosRelative(seg->frontsector->PortalGroup);
				float x = posrel.X;
				float y = posrel.Y;
				float z = posrel.Z;
				float dist = fabsf(p.DistToPoint(x, z, y));
				float radius = lightsource->GetRadius();
				float scale = 1.0f / ((2.f * radius) - dist);
				FVector3 fn, pos;

				if (radius > 0.f && dist < radius)
				{
					FVector3 nearPt, up, right;

					pos = { x, z, y };
					fn = p.Normal();

					fn.GetRightUp(right, up);

					FVector3 tmpVec = fn * dist;
					nearPt = pos + tmpVec;

					FVector3 t1;
					int outcnt[4]={0,0,0,0};
					texcoord tcs[4];

					// do a quick check whether the light touches this polygon
					for(int i=0;i<4;i++)
					{
						t1 = FVector3(&vtx[i*3]);
						FVector3 nearToVert = t1 - nearPt;
						tcs[i].u = ((nearToVert | right) * scale) + 0.5f;
						tcs[i].v = ((nearToVert | up) * scale) + 0.5f;

						if (tcs[i].u<0) outcnt[0]++;
						if (tcs[i].u>1) outcnt[1]++;
						if (tcs[i].v<0) outcnt[2]++;
						if (tcs[i].v>1) outcnt[3]++;

					}
					if (outcnt[0]!=4 && outcnt[1]!=4 && outcnt[2]!=4 && outcnt[3]!=4)
					{
						draw_dlight += GetLight(lightdata, seg->frontsector->PortalGroup, p, lightsource, true);
					}
				}
			}
		}
	}

	dynlightindex = screen->mLights->UploadLights(lightdata);
}


const char HWWall::passflag[] = {
	0,		//RENDERWALL_NONE,
	1,		//RENDERWALL_TOP,              // unmasked
	1,		//RENDERWALL_M1S,              // unmasked
	2,		//RENDERWALL_M2S,              // depends on render and texture settings
	1,		//RENDERWALL_BOTTOM,           // unmasked
	3,		//RENDERWALL_FOGBOUNDARY,      // translucent
	1,		//RENDERWALL_MIRRORSURFACE,    // only created from PORTALTYPE_MIRROR
	2,		//RENDERWALL_M2SNF,            // depends on render and texture settings, no fog, used on mid texture lines with a fog boundary.
	3,		//RENDERWALL_COLOR,            // translucent
	2,		//RENDERWALL_FFBLOCK           // depends on render and texture settings
};

//==========================================================================
//
//
//
//==========================================================================
static void CaptureGpuPersistentWall(HWWall* wall);
static void RejectGpuPersistentWall(HWWall* wall);

void HWWall::PutWall(HWWallDispatcher *di, bool translucent)
{
	if (texture && texture->GetTranslucency() && passflag[type] == 2)
	{
		translucent = true;
	}

	auto ddi = di->di;
	if (translucent)
	{
		flags |= HWF_TRANSLUCENT;
	}

	if (di->di)
	{
		if (translucent)
		{
			ViewDistance = (ddi->Viewpoint.Pos - (seg->linedef->v1->fPos() + seg->linedef->Delta() / 2)).XY().LengthSquared();
		}

		if (ddi->isFullbrightScene())
		{
			// light planes don't get drawn with fullbright rendering
			if (texture == NULL) return;
			Colormap.Clear();
		}

		if (ddi->isFullbrightScene() || (Colormap.LightColor.isWhite() && lightlevel == 255))
		{
			flags &= ~HWF_GLOW;
		}

		if (!screen->BuffersArePersistent())
		{
			if (ddi->Level->HasDynamicLights && !ddi->isFullbrightScene() && texture != nullptr)
			{
				SetupLights(ddi, lightdata);
			}
			MakeVertices(translucent);
		}



		bool solid;
		if (passflag[type] == 1) solid = true;
		else if (type == RENDERWALL_FFBLOCK) solid = texture && !texture->isMasked();
		else solid = false;

		bool hasDecals = solid && seg->sidedef && seg->sidedef->AttachedDecals;
		if (hasDecals)
		{
			// If we want to use the light infos for the decal we cannot delay the creation until the render pass.
			if (screen->BuffersArePersistent())
			{
				if (ddi->Level->HasDynamicLights && !ddi->isFullbrightScene() && texture != nullptr)
				{
					SetupLights(ddi, lightdata);
				}
			}
			ProcessDecals(ddi);
		}
	}


	if (SceneProfiling && !di->SuppressSubmit)
	{
		auto &profile = GetSceneSurfaceProfile();
		++profile.Submitted;
		if (translucent) ++profile.Translucent;
		if (texture && texture->isMasked()) ++profile.Masked;
		if (seg->sidedef && seg->sidedef->AttachedDecals) ++profile.WithDecals;
		if (!translucent && texture && !texture->isMasked() &&
			(type == RENDERWALL_TOP || type == RENDERWALL_M1S || type == RENDERWALL_BOTTOM))
		{
			++profile.OpaqueRegular;
			if (type == RENDERWALL_TOP) ++profile.OutOrdinaryTop;
			else if (type == RENDERWALL_BOTTOM) ++profile.OutOrdinaryBottom;
			else ++profile.OutOrdinaryM1S;
		}
		if (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF) ++profile.OutM2S;
	}
	if (GpuOwnerCandidate) CaptureGpuPersistentWall(this);
	CapturePreparedWall(di, this);
	di->AddWall(this);
	Prepared = nullptr;

	lightlist = nullptr;
	// make sure that following parts of the same linedef do not get this one's vertex and lighting info.
	vertcount = 0;
	dynlightindex = -1;
	flags &= ~HWF_TRANSLUCENT;
}

//==========================================================================
//
//
//
//==========================================================================

void HWWall::PutPortal(HWWallDispatcher *di, int ptype, int plane)
{
	if (di->SuppressSubmit) return;
	RejectGpuPersistentWall(this);
	HWPortal * portal = nullptr;

	auto ddi = di->di;
	if (ddi)
	{
		MakeVertices(false);
		switch (ptype)
		{
			// portals don't go into the draw list.
			// Instead they are added to the portal manager
		case PORTALTYPE_HORIZON:
			horizon = portalState.UniqueHorizons.Get(horizon);
			portal = ddi->FindPortal(horizon);
			if (!portal)
			{
				portal = new HWHorizonPortal(&portalState, horizon, ddi->Viewpoint);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_SKYBOX:
			portal = ddi->FindPortal(secportal);
			if (!portal)
			{
				// either a regular skybox or an Eternity-style horizon
				if (secportal->mType != PORTS_SKYVIEWPOINT) portal = new HWEEHorizonPortal(&portalState, secportal);
				else
				{
					portal = new HWSkyboxPortal(&portalState, secportal);
					ddi->Portals.Push(portal);
				}
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_SECTORSTACK:
			portal = ddi->FindPortal(this->portal);
			if (!portal)
			{
				portal = new HWSectorStackPortal(&portalState, this->portal);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_PLANEMIRROR:
			if (ddi->Viewpoint.bDoOrtho ? (ddi->Viewpoint.ViewVector3D.dot(planemirror->Normal()) < 0)
				: (portalState.PlaneMirrorMode * planemirror->fC() <= 0))
			{
				planemirror = portalState.UniquePlaneMirrors.Get(planemirror);
				portal = ddi->FindPortal(planemirror);
				if (!portal)
				{
					portal = new HWPlaneMirrorPortal(&portalState, planemirror);
					ddi->Portals.Push(portal);
				}
				portal->AddLine(this);
			}
			break;

		case PORTALTYPE_MIRROR:
			portal = ddi->FindPortal(seg->linedef);
			if (!portal)
			{
				portal = new HWMirrorPortal(&portalState, seg->linedef);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			if (gl_mirror_envmap)
			{
				// draw a reflective layer over the mirror
				ddi->AddMirrorSurface(this);
			}
			break;

		case PORTALTYPE_LINETOLINE:
			if (!lineportal)
				return;
			portal = ddi->FindPortal(lineportal);
			if (!portal)
			{
				line_t* otherside = lineportal->lines[0]->mDestination;
				if (otherside != nullptr && otherside->portalindex < ddi->Level->linePortals.Size())
				{
					ddi->ProcessActorsInPortal(otherside->getPortal()->mGroup, ddi->in_area);
				}
				portal = new HWLineToLinePortal(&portalState, lineportal);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_SKY:
			sky = portalState.UniqueSkies.Get(sky);
			portal = ddi->FindPortal(sky);
			if (!portal)
			{
				portal = new HWSkyPortal(screen->mSkyData, &portalState, sky);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;
		}
		vertcount = 0;

		if (plane != -1 && portal)
		{
			portal->planesused |= (1 << plane);
		}
	}
	else
	{
		portaltype = ptype;
		portalplane = plane;
		di->AddPortal(this);
	}
}

//==========================================================================
//
//	Sets 3D-floor lighting info
//
//==========================================================================

void HWWall::Put3DWall(HWWallDispatcher *di, lightlist_t * lightlist, bool translucent)
{
	// only modify the light di->Level-> if it doesn't originate from the seg's frontsector. This is to account for light transferring effects
	if (lightlist->p_lightlevel != &seg->sidedef->sector->lightlevel)
	{
		lightlevel = RescaleLightLevel(*lightlist->p_lightlevel);
	}
	// relative light won't get changed here. It is constant across the entire wall.

	CopyFrom3DLight(Colormap, lightlist);
	PutWall(di, translucent);
}

//==========================================================================
//
//  Splits a wall vertically if a 3D-floor
//	creates different lighting across the wall
//
//==========================================================================

bool HWWall::SplitWallComplex(HWWallDispatcher *di, sector_t * frontsector, bool translucent, float& maplightbottomleft, float& maplightbottomright)
{
	// check for an intersection with the upper plane
	if ((maplightbottomleft<ztop[0] && maplightbottomright>ztop[1]) ||
		(maplightbottomleft>ztop[0] && maplightbottomright<ztop[1]))
	{
		float clen = max<float>(fabsf(glseg.x2 - glseg.x1), fabsf(glseg.y2 - glseg.y1));

		float dch = ztop[1] - ztop[0];
		float dfh = maplightbottomright - maplightbottomleft;
		float coeff = (ztop[0] - maplightbottomleft) / (dfh - dch);

		// check for inaccuracies - let's be a little generous here!
		if (coeff*clen<.1f)
		{
			maplightbottomleft = ztop[0];
		}
		else if (coeff*clen>clen - .1f)
		{
			maplightbottomright = ztop[1];
		}
		else
		{
			// split the wall in two at the intersection and recursively split both halves
			HWWall copyWall1 = *this, copyWall2 = *this;

			copyWall1.glseg.x2 = copyWall2.glseg.x1 = glseg.x1 + coeff * (glseg.x2 - glseg.x1);
			copyWall1.glseg.y2 = copyWall2.glseg.y1 = glseg.y1 + coeff * (glseg.y2 - glseg.y1);
			copyWall1.ztop[1] = copyWall2.ztop[0] = ztop[0] + coeff * (ztop[1] - ztop[0]);
			copyWall1.zbottom[1] = copyWall2.zbottom[0] = zbottom[0] + coeff * (zbottom[1] - zbottom[0]);
			copyWall1.glseg.fracright = copyWall2.glseg.fracleft = glseg.fracleft + coeff * (glseg.fracright - glseg.fracleft);
			copyWall1.tcs[UPRGT].u = copyWall2.tcs[UPLFT].u = tcs[UPLFT].u + coeff * (tcs[UPRGT].u - tcs[UPLFT].u);
			copyWall1.tcs[UPRGT].v = copyWall2.tcs[UPLFT].v = tcs[UPLFT].v + coeff * (tcs[UPRGT].v - tcs[UPLFT].v);
			copyWall1.tcs[LORGT].u = copyWall2.tcs[LOLFT].u = tcs[LOLFT].u + coeff * (tcs[LORGT].u - tcs[LOLFT].u);
			copyWall1.tcs[LORGT].v = copyWall2.tcs[LOLFT].v = tcs[LOLFT].v + coeff * (tcs[LORGT].v - tcs[LOLFT].v);
			copyWall1.lightuv[UPRGT].u = copyWall2.lightuv[UPLFT].u = lightuv[UPLFT].u + coeff * (lightuv[UPRGT].u - lightuv[UPLFT].u);
			copyWall1.lightuv[UPRGT].v = copyWall2.lightuv[UPLFT].v = lightuv[UPLFT].v + coeff * (lightuv[UPRGT].v - lightuv[UPLFT].v);
			copyWall1.lightuv[LORGT].u = copyWall2.lightuv[LOLFT].u = lightuv[LOLFT].u + coeff * (lightuv[LORGT].u - lightuv[LOLFT].u);
			copyWall1.lightuv[LORGT].v = copyWall2.lightuv[LOLFT].v = lightuv[LOLFT].v + coeff * (lightuv[LORGT].v - lightuv[LOLFT].v);

			copyWall1.SplitWall(di, frontsector, translucent);
			copyWall2.SplitWall(di, frontsector, translucent);
			return true;
		}
	}

	// check for an intersection with the lower plane
	if ((maplightbottomleft<zbottom[0] && maplightbottomright>zbottom[1]) ||
		(maplightbottomleft>zbottom[0] && maplightbottomright<zbottom[1]))
	{
		float clen = max<float>(fabsf(glseg.x2 - glseg.x1), fabsf(glseg.y2 - glseg.y1));

		float dch = zbottom[1] - zbottom[0];
		float dfh = maplightbottomright - maplightbottomleft;
		float coeff = (zbottom[0] - maplightbottomleft) / (dfh - dch);

		// check for inaccuracies - let's be a little generous here because there's
		// some conversions between floats and fixed_t's involved
		if (coeff*clen<.1f)
		{
			maplightbottomleft = zbottom[0];
		}
		else if (coeff*clen>clen - .1f)
		{
			maplightbottomright = zbottom[1];
		}
		else
		{
			// split the wall in two at the intersection and recursively split both halves
			HWWall copyWall1 = *this, copyWall2 = *this;

			copyWall1.glseg.x2 = copyWall2.glseg.x1 = glseg.x1 + coeff * (glseg.x2 - glseg.x1);
			copyWall1.glseg.y2 = copyWall2.glseg.y1 = glseg.y1 + coeff * (glseg.y2 - glseg.y1);
			copyWall1.ztop[1] = copyWall2.ztop[0] = ztop[0] + coeff * (ztop[1] - ztop[0]);
			copyWall1.zbottom[1] = copyWall2.zbottom[0] = zbottom[0] + coeff * (zbottom[1] - zbottom[0]);
			copyWall1.glseg.fracright = copyWall2.glseg.fracleft = glseg.fracleft + coeff * (glseg.fracright - glseg.fracleft);
			copyWall1.tcs[UPRGT].u = copyWall2.tcs[UPLFT].u = tcs[UPLFT].u + coeff * (tcs[UPRGT].u - tcs[UPLFT].u);
			copyWall1.tcs[UPRGT].v = copyWall2.tcs[UPLFT].v = tcs[UPLFT].v + coeff * (tcs[UPRGT].v - tcs[UPLFT].v);
			copyWall1.tcs[LORGT].u = copyWall2.tcs[LOLFT].u = tcs[LOLFT].u + coeff * (tcs[LORGT].u - tcs[LOLFT].u);
			copyWall1.tcs[LORGT].v = copyWall2.tcs[LOLFT].v = tcs[LOLFT].v + coeff * (tcs[LORGT].v - tcs[LOLFT].v);
			copyWall1.lightuv[UPRGT].u = copyWall2.lightuv[UPLFT].u = lightuv[UPLFT].u + coeff * (lightuv[UPRGT].u - lightuv[UPLFT].u);
			copyWall1.lightuv[UPRGT].v = copyWall2.lightuv[UPLFT].v = lightuv[UPLFT].v + coeff * (lightuv[UPRGT].v - lightuv[UPLFT].v);
			copyWall1.lightuv[LORGT].u = copyWall2.lightuv[LOLFT].u = lightuv[LOLFT].u + coeff * (lightuv[LORGT].u - lightuv[LOLFT].u);
			copyWall1.lightuv[LORGT].v = copyWall2.lightuv[LOLFT].v = lightuv[LOLFT].v + coeff * (lightuv[LORGT].v - lightuv[LOLFT].v);

			copyWall1.SplitWall(di, frontsector, translucent);
			copyWall2.SplitWall(di, frontsector, translucent);
			return true;
		}
	}

	return false;
}

void HWWall::SplitWall(HWWallDispatcher *di, sector_t * frontsector, bool translucent)
{
	float maplightbottomleft;
	float maplightbottomright;
	unsigned int i;
	int origlight = lightlevel;
	FColormap origcm=Colormap;

	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	if (glseg.x1==glseg.x2 && glseg.y1==glseg.y2)
	{
		return;
	}
	//::SplitWall.Clock();

#ifdef _DEBUG
	if (seg->linedef->Index() == 1)
	{
		int a = 0;
	}
#endif

	if (lightlist.Size()>1)
	{
		for(i=0;i<lightlist.Size()-1;i++)
		{
			if (i<lightlist.Size()-1)
			{
				secplane_t &p = lightlist[i+1].plane;
				maplightbottomleft = p.ZatPoint(glseg.x1,glseg.y1);
				maplightbottomright= p.ZatPoint(glseg.x2,glseg.y2);
			}
			else
			{
				maplightbottomright = maplightbottomleft = -32000;
			}

			// The light is completely above the wall!
			if (maplightbottomleft>=ztop[0] && maplightbottomright>=ztop[1])
			{
				continue;
			}

			// check for an intersection with the upper and lower planes of the wall segment
			if ((maplightbottomleft<ztop[0] && maplightbottomright>ztop[1]) ||
				(maplightbottomleft > ztop[0] && maplightbottomright < ztop[1]) ||
				(maplightbottomleft<zbottom[0] && maplightbottomright>zbottom[1]) ||
				(maplightbottomleft > zbottom[0] && maplightbottomright < zbottom[1]))
			{
				if (!(screen->hwcaps & RFL_NO_CLIP_PLANES))
				{
					// Use hardware clipping if this cannot be done cleanly.
					this->lightlist = &lightlist;
					PutWall(di, translucent);

					goto out;
				}
				// crappy fallback if no clip planes available
				else if (SplitWallComplex(di, frontsector, translucent, maplightbottomleft, maplightbottomright))
				{
					goto out;
				}
			}

			// 3D floor is completely within this light
			if (maplightbottomleft<=zbottom[0] && maplightbottomright<=zbottom[1])
			{
				Put3DWall(di, &lightlist[i], translucent);
				goto out;
			}

			if (maplightbottomleft<=ztop[0] && maplightbottomright<=ztop[1] &&
				(maplightbottomleft!=ztop[0] || maplightbottomright!=ztop[1]))
			{
				HWWall copyWall1 = *this;

				copyWall1.flags |= HWF_NOSPLITLOWER;
				flags |= HWF_NOSPLITUPPER;
				ztop[0]=copyWall1.zbottom[0]=maplightbottomleft;
				ztop[1]=copyWall1.zbottom[1]=maplightbottomright;
				tcs[UPLFT].v=copyWall1.tcs[LOLFT].v=copyWall1.tcs[UPLFT].v+
					(maplightbottomleft-copyWall1.ztop[0])*(copyWall1.tcs[LOLFT].v-copyWall1.tcs[UPLFT].v)/(zbottom[0]-copyWall1.ztop[0]);
				tcs[UPRGT].v=copyWall1.tcs[LORGT].v=copyWall1.tcs[UPRGT].v+
					(maplightbottomright-copyWall1.ztop[1])*(copyWall1.tcs[LORGT].v-copyWall1.tcs[UPRGT].v)/(zbottom[1]-copyWall1.ztop[1]);
				lightuv[UPLFT].v=copyWall1.lightuv[LOLFT].v=copyWall1.lightuv[UPLFT].v+
					(maplightbottomleft-copyWall1.ztop[0])*(copyWall1.lightuv[LOLFT].v-copyWall1.lightuv[UPLFT].v)/(zbottom[0]-copyWall1.ztop[0]);
				lightuv[UPRGT].v=copyWall1.lightuv[LORGT].v=copyWall1.lightuv[UPRGT].v+
					(maplightbottomright-copyWall1.ztop[1])*(copyWall1.lightuv[LORGT].v-copyWall1.lightuv[UPRGT].v)/(zbottom[1]-copyWall1.ztop[1]);
				copyWall1.Put3DWall(di, &lightlist[i], translucent);
			}
			if (ztop[0]==zbottom[0] && ztop[1]==zbottom[1])
			{
				//::SplitWall.Unclock();
				goto out;
			}
		}
	}

	Put3DWall(di, &lightlist[lightlist.Size()-1], translucent);

out:
	lightlevel=origlight;
	Colormap=origcm;
	flags &= ~HWF_NOSPLITUPPER;
	this->lightlist = NULL;
	//::SplitWall.Unclock();
}


//==========================================================================
//
//
//
//==========================================================================
bool HWWall::DoHorizon(HWWallDispatcher *di, seg_t * seg,sector_t * fs, vertex_t * v1,vertex_t * v2)
{
	HWHorizonInfo hi;
	lightlist_t * light;

	// ZDoom doesn't support slopes in a horizon sector so I won't either!
	ztop[1] = ztop[0] = fs->GetPlaneTexZ(sector_t::ceiling);
	zbottom[1] = zbottom[0] = fs->GetPlaneTexZ(sector_t::floor);

	auto ddi = di->di;
	if (ddi)
	{
		auto vpz = ddi->Viewpoint.Pos.Z;
		if (vpz < fs->GetPlaneTexZ(sector_t::ceiling))
		{
			if (vpz > fs->GetPlaneTexZ(sector_t::floor))
				zbottom[1] = zbottom[0] = vpz;

			if (fs->GetTexture(sector_t::ceiling) == skyflatnum)
			{
				SkyPlane(di, fs, sector_t::ceiling, false);
			}
			else
			{
				hi.plane.GetFromSector(fs, sector_t::ceiling);
				hi.lightlevel = RescaleLightLevel(fs->GetCeilingLight());
				hi.colormap = fs->Colormap;
				hi.specialcolor = fs->SpecialColors[sector_t::ceiling];

				if (fs->e->XFloor.ffloors.Size())
				{
					light = P_GetPlaneLight(fs, &fs->ceilingplane, true);

					if (!(fs->GetFlags(sector_t::ceiling) & PLANEF_ABSLIGHTING)) hi.lightlevel = RescaleLightLevel(*light->p_lightlevel);
					hi.colormap.CopyLight(light->extra_colormap);
				}

				if (ddi->isFullbrightScene()) hi.colormap.Clear();
				horizon = &hi;
				PutPortal(di, PORTALTYPE_HORIZON, -1);
			}
			ztop[1] = ztop[0] = zbottom[0];
		}

		if (vpz > fs->GetPlaneTexZ(sector_t::floor))
		{
			zbottom[1] = zbottom[0] = fs->GetPlaneTexZ(sector_t::floor);
			if (fs->GetTexture(sector_t::floor) == skyflatnum)
			{
				SkyPlane(di, fs, sector_t::floor, false);
			}
			else
			{
				hi.plane.GetFromSector(fs, sector_t::floor);
				hi.lightlevel = RescaleLightLevel(fs->GetFloorLight());
				hi.colormap = fs->Colormap;
				hi.specialcolor = fs->SpecialColors[sector_t::floor];

				if (fs->e->XFloor.ffloors.Size())
				{
					light = P_GetPlaneLight(fs, &fs->floorplane, false);

					if (!(fs->GetFlags(sector_t::floor) & PLANEF_ABSLIGHTING)) hi.lightlevel = RescaleLightLevel(*light->p_lightlevel);
					hi.colormap.CopyLight(light->extra_colormap);
				}

				if (ddi->isFullbrightScene()) hi.colormap.Clear();
				horizon = &hi;
				PutPortal(di, PORTALTYPE_HORIZON, -1);
			}
		}
	}
	else
	{
		// we cannot build the real portal yet, the mesh builder just needs a generic 'horizon' portal that needs to be filled in in the render pass.
		PutPortal(di, PORTALTYPE_HORIZON, -1);
	}
	return true;
}

static float ZeroLightmapUVs[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

//==========================================================================
//
//
//
//==========================================================================
bool HWWall::SetWallCoordinates(seg_t * seg, FTexCoordInfo *tci, float texturetop,
	float topleft, float topright, float bottomleft, float bottomright, float t_ofs, float skew)
{
	//
	//
	// set up texture coordinate stuff
	//
	//
	float l_ul;
	float texlength;

	if (texture)
	{
		float length = seg->sidedef ? seg->sidedef->TexelLength : Dist2(glseg.x1, glseg.y1, glseg.x2, glseg.y2);

		l_ul = tci->FloatToTexU(tci->TextureOffset(t_ofs));
		texlength = tci->FloatToTexU(length);
	}
	else
	{
		tci = NULL;
		l_ul = 0;
		texlength = 0;
	}

	texcoord* srclightuv;
	if (lightmap && lightmap->Type != ST_NULL)
	{
		srclightuv = (texcoord*)lightmap->TexCoords;
		lindex = (float)lightmap->LightmapNum;
	}
	else
	{
		srclightuv = (texcoord*)ZeroLightmapUVs;
		lindex = -1.0f;
	}

	//
	//
	// set up coordinates for the left side of the polygon
	//
	// check left side for intersections
	if (topleft >= bottomleft)
	{
		// normal case
		ztop[0] = topleft;
		zbottom[0] = bottomleft;

		if (tci)
		{
			tcs[UPLFT].v = tci->FloatToTexV(-ztop[0] + texturetop);
			tcs[LOLFT].v = tci->FloatToTexV(-zbottom[0] + texturetop);
		}

		lightuv[UPLFT].v = srclightuv[UPLFT].v;
		lightuv[LOLFT].v = srclightuv[LOLFT].v;
	}
	else
	{
		// ceiling below floor - clip to the visible part of the wall
		HeightValid = false;
		float dch = topright - topleft;
		float dfh = bottomright - bottomleft;
		float inter_x = (bottomleft - topleft) / (dch - dfh);

		float inter_y = topleft + inter_x * dch;

		glseg.x1 = glseg.x1 + inter_x * (glseg.x2 - glseg.x1);
		glseg.y1 = glseg.y1 + inter_x * (glseg.y2 - glseg.y1);
		glseg.fracleft = inter_x;

		zbottom[0] = ztop[0] = inter_y;

		if (tci)
		{
			tcs[LOLFT].v = tcs[UPLFT].v = tci->FloatToTexV(-ztop[0] + texturetop);
		}

		lightuv[UPLFT].v = srclightuv[UPLFT].v + inter_x * (srclightuv[UPRGT].v - srclightuv[UPLFT].v);
		lightuv[LOLFT].v = srclightuv[LOLFT].v + inter_x * (srclightuv[LORGT].v - srclightuv[LOLFT].v);
	}

	//
	//
	// set up coordinates for the right side of the polygon
	//
	// check left side for intersections
	if (topright >= bottomright)
	{
		// normal case
		ztop[1] = topright;
		zbottom[1] = bottomright;

		if (tci)
		{
			tcs[UPRGT].v = tci->FloatToTexV(-ztop[1] + texturetop + skew);
			tcs[LORGT].v = tci->FloatToTexV(-zbottom[1] + texturetop + skew);
		}

		lightuv[UPRGT].v = srclightuv[UPRGT].v;
		lightuv[LORGT].v = srclightuv[LORGT].v;
	}
	else
	{
		// ceiling below floor - clip to the visible part of the wall
		HeightValid = false;
		float dch = topright - topleft;
		float dfh = bottomright - bottomleft;
		float inter_x = (bottomleft - topleft) / (dch - dfh);

		float inter_y = topleft + inter_x * dch;

		glseg.x2 = glseg.x1 + inter_x * (glseg.x2 - glseg.x1);
		glseg.y2 = glseg.y1 + inter_x * (glseg.y2 - glseg.y1);
		glseg.fracright = inter_x;

		zbottom[1] = ztop[1] = inter_y;
		if (tci)
		{
			tcs[LORGT].v = tcs[UPRGT].v = tci->FloatToTexV(-ztop[1] + texturetop + skew);
		}

		lightuv[UPRGT].v = srclightuv[UPRGT].v + inter_x * (srclightuv[UPRGT].v - srclightuv[UPLFT].v);
		lightuv[LORGT].v = srclightuv[LORGT].v + inter_x * (srclightuv[LORGT].v - srclightuv[LOLFT].v);
	}

	tcs[UPLFT].u = tcs[LOLFT].u = l_ul + texlength * glseg.fracleft;
	tcs[UPRGT].u = tcs[LORGT].u = l_ul + texlength * glseg.fracright;

	lightuv[UPLFT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracleft;
	lightuv[LOLFT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracleft;
	lightuv[UPRGT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracright;
	lightuv[LORGT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracright;

	if (texture != NULL)
	{
		bool normalize = false;
		if (texture->isHardwareCanvas()) normalize = true;
		else if (flags & HWF_CLAMPY)
		{
			// for negative scales we can get negative coordinates here.
			normalize = (tcs[UPLFT].v > tcs[LOLFT].v || tcs[UPRGT].v > tcs[LORGT].v);
		}
		if (normalize)
		{
			// we have to shift the y-coordinate from [-1..0] to [0..1] when using texture clamping with a negative scale
			tcs[UPLFT].v += 1.f;
			tcs[UPRGT].v += 1.f;
			tcs[LOLFT].v += 1.f;
			tcs[LORGT].v += 1.f;
		}
	}

	return true;
}

//==========================================================================
//
// Do some tweaks with the texture coordinates to reduce visual glitches
//
//==========================================================================

void HWWall::CheckTexturePosition(FTexCoordInfo *tci)
{
	float sub;

	if (texture->isHardwareCanvas()) return;

	// clamp texture coordinates to a reasonable range.
	// Extremely large values can cause visual problems
	if (tci->mScale.Y > 0)
	{
		if (tcs[UPLFT].v < tcs[UPRGT].v)
		{
			sub = float(RoundDown(tcs[UPLFT].v));
		}
		else
		{
			sub = float(RoundDown(tcs[UPRGT].v));
		}
		tcs[UPLFT].v -= sub;
		tcs[UPRGT].v -= sub;
		tcs[LOLFT].v -= sub;
		tcs[LORGT].v -= sub;

		if ((tcs[UPLFT].v == 0.f && tcs[UPRGT].v == 0.f && tcs[LOLFT].v <= 1.f && tcs[LORGT].v <= 1.f) ||
			(tcs[UPLFT].v >= 0.f && tcs[UPRGT].v >= 0.f && tcs[LOLFT].v == 1.f && tcs[LORGT].v == 1.f))
		{
			flags |= HWF_CLAMPY;
		}
	}
	else
	{
		if (tcs[LOLFT].v < tcs[LORGT].v)
		{
			sub = float(RoundDown(tcs[LOLFT].v));
		}
		else
		{
			sub = float(RoundDown(tcs[LORGT].v));
		}
		tcs[UPLFT].v -= sub;
		tcs[UPRGT].v -= sub;
		tcs[LOLFT].v -= sub;
		tcs[LORGT].v -= sub;

		if ((tcs[LOLFT].v == 0.f && tcs[LORGT].v == 0.f && tcs[UPLFT].v <= 1.f && tcs[UPRGT].v <= 1.f) ||
			(tcs[LOLFT].v >= 0.f && tcs[LORGT].v >= 0.f && tcs[UPLFT].v == 1.f && tcs[UPRGT].v == 1.f))
		{
			flags |= HWF_CLAMPY;
		}
	}

	// Check if this is marked as a skybox and if so, do the same for x.
	// This intentionally only tests the seg's frontsector.
	if (seg->frontsector->special == GLSector_Skybox)
	{
		sub = (float)RoundDown(tcs[UPLFT].u);
		tcs[UPLFT].u -= sub;
		tcs[UPRGT].u -= sub;
		tcs[LOLFT].u -= sub;
		tcs[LORGT].u -= sub;
		if ((tcs[UPLFT].u == 0.f && tcs[LOLFT].u == 0.f && tcs[UPRGT].u <= 1.f && tcs[LORGT].u <= 1.f) ||
			(tcs[UPLFT].u >= 0.f && tcs[LOLFT].u >= 0.f && tcs[UPRGT].u == 1.f && tcs[LORGT].u == 1.f))
		{
			flags |= HWF_CLAMPX;
		}
	}
}


static void GetTexCoordInfo(FGameTexture *tex, FTexCoordInfo *tci, side_t *side, int texpos)
{
	tci->GetFromTexture(tex, (float)side->GetTextureXScale(texpos), (float)side->GetTextureYScale(texpos), !!(side->GetLevel()->flags3 & LEVEL3_FORCEWORLDPANNING));
}

//==========================================================================
//
//  Handle one sided walls, upper and lower texture
//
//==========================================================================
void HWWall::DoTexture(HWWallDispatcher *di, int _type,seg_t * seg, int peg,
					   float ceilingrefheight,float floorrefheight,
					   float topleft,float topright,
					   float bottomleft,float bottomright,
					   float v_offset, float skew)
{
	if (topleft<=bottomleft && topright<=bottomright) return;

	// The Vertex values can be destroyed in this function and must be restored aferward!
	HWSeg glsave=glseg;
	float flh=ceilingrefheight-floorrefheight;
	int texpos;
	uint8_t savedflags = flags;

	switch (_type)
	{
	case RENDERWALL_TOP:
		texpos = side_t::top;
		break;
	case RENDERWALL_BOTTOM:
		texpos = side_t::bottom;
		break;
	default:
		texpos = side_t::mid;
		break;
	}

	FTexCoordInfo tci;

	GetTexCoordInfo(texture, &tci, seg->sidedef, texpos);
	HeightValid = HeightKnown = HeightArmed;
	HeightArmed = false;
	HeightVScale = tci.FloatToTexV(1.f);

	type = _type;

	if (seg->sidedef->lightmap && type >= RENDERWALL_TOP && type <= RENDERWALL_BOTTOM)
	{
		lightmap = &seg->sidedef->lightmap[type - RENDERWALL_TOP];
	}
	else
	{
		lightmap = nullptr;
	}

	float floatceilingref = ceilingrefheight + tci.RowOffset(seg->sidedef->GetTextureYOffset(texpos));
	if (peg) floatceilingref += tci.mRenderHeight - flh - v_offset;

	if (!SetWallCoordinates(seg, &tci, floatceilingref, topleft, topright, bottomleft, bottomright,
							seg->sidedef->GetTextureXOffset(texpos), skew)) return;

	if (seg->linedef->special == Line_Mirror && _type == RENDERWALL_M1S && gl_mirrors && !(di->Level->ib_compatflags & BCOMPATF_NOMIRRORS))
	{
		PutPortal(di, PORTALTYPE_MIRROR, -1);
	}
	else
	{

		CheckTexturePosition(&tci);
		// [Karto] Glow planes are baked into the row. A clamped tier stays on the records; world mode guards it on the CPU
		// and re-produces it once a move takes its texture range out of [0, 1], where the producer would drop the clamp.
		if (flags & HWF_GLOW) HeightValid = false;

		// Add this wall to the render list
		sector_t * sec = sub ? sub->sector : seg->frontsector;
		if (sec->e->XFloor.lightlist.Size()==0 ||  di->isFullbrightScene()) PutWall(di, false);
		else
		{
			HeightValid = false;
			SplitWall(di, sec, false);
		}
	}

	HeightValid = HeightKnown = false;
	glseg = glsave;
	flags = savedflags;
}


//==========================================================================
//
//
//
//==========================================================================

void HWWall::DoMidTexture(HWWallDispatcher *di, seg_t * seg, bool drawfogboundary,
						  sector_t * front, sector_t * back,
						  sector_t * realfront, sector_t * realback,
						  float fch1, float fch2, float ffh1, float ffh2,
						  float bch1, float bch2, float bfh1, float bfh2, float zalign, float skew)

{
	FTexCoordInfo tci;
	float topleft,bottomleft,topright,bottomright;
	HWSeg glsave=glseg;
	float texturetop, texturebottom;
	bool wrap = (seg->linedef->flags&ML_WRAP_MIDTEX) || (seg->sidedef->Flags&WALLF_WRAP_MIDTEX);
	bool mirrory = false;
	float rowoffset = 0;

	//
	//
	// Get the base coordinates for the texture
	//
	//
	if (texture)
	{
		// Align the texture to the ORIGINAL sector's height!!
		// At this point slopes don't matter because they don't affect the texture's z-position

		GetTexCoordInfo(texture, &tci, seg->sidedef, side_t::mid);
		if (tci.mRenderHeight < 0)
		{
			mirrory = true;
			tci.mRenderHeight = -tci.mRenderHeight;
			tci.mScale.Y = -tci.mScale.Y;
		}
		rowoffset = tci.RowOffset(seg->sidedef->GetTextureYOffset(side_t::mid));
		if ((seg->linedef->flags & ML_DONTPEGBOTTOM) >0)
		{
			texturebottom = max(realfront->GetPlaneTexZ(sector_t::floor), realback->GetPlaneTexZ(sector_t::floor) + zalign) + rowoffset;
			texturetop = texturebottom + tci.mRenderHeight;
		}
		else
		{
			texturetop = min(realfront->GetPlaneTexZ(sector_t::ceiling), realback->GetPlaneTexZ(sector_t::ceiling) + zalign) + rowoffset;
			texturebottom = texturetop - tci.mRenderHeight;
		}
	}
	else texturetop=texturebottom=0;

	//
	//
	// Depending on missing textures and possible plane intersections
	// decide which planes to use for the polygon
	//
	//
	if (realfront!=realback || drawfogboundary || wrap || realfront->GetHeightSec())
	{
		//
		//
		// Set up the top
		//
		//
		auto tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::top), true);
		if (!tex || !tex->isValid())
		{
			if (front->GetTexture(sector_t::ceiling) == skyflatnum &&
				back->GetTexture(sector_t::ceiling) == skyflatnum && !wrap && skew == 0)
			{
				// intra-sky lines do not clip the texture at all if there's no upper texture.
				topleft = topright = texturetop;
			}
			else
			{
				// texture is missing - use the higher plane
				topleft = max(bch1,fch1);
				topright = max(bch2,fch2);
			}
		}
		else if ((bch1>fch1 || bch2>fch2) &&
				 (seg->frontsector->GetTexture(sector_t::ceiling)!=skyflatnum || seg->backsector->GetTexture(sector_t::ceiling)==skyflatnum))
				 // (!((bch1<=fch1 && bch2<=fch2) || (bch1>=fch1 && bch2>=fch2)))
		{
			// Use the higher plane and let the geometry clip the extruding part
			topleft = bch1;
			topright = bch2;
		}
		else
		{
			// But not if there can be visual artifacts.
			topleft = min(bch1,fch1);
			topright = min(bch2,fch2);
		}

		//
		//
		// Set up the bottom
		//
		//
		tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::bottom), true);
		if (!tex || !tex->isValid())
		{
			// texture is missing - use the lower plane
			bottomleft = min(bfh1,ffh1);
			bottomright = min(bfh2,ffh2);
		}
		else if (bfh1<ffh1 || bfh2<ffh2) // (!((bfh1<=ffh1 && bfh2<=ffh2) || (bfh1>=ffh1 && bfh2>=ffh2)))
		{
			// the floor planes intersect. Use the backsector's floor for drawing so that
			// drawing the front sector's plane clips the polygon automatically.
			bottomleft = bfh1;
			bottomright = bfh2;
		}
		else
		{
			// normal case - use the higher plane
			bottomleft = max(bfh1,ffh1);
			bottomright = max(bfh2,ffh2);
		}

		//
		//
		// if we don't need a fog sheet let's clip away some unnecessary parts of the polygon
		//
		//
		if (!drawfogboundary && !wrap && skew == 0)
		{
			if (texturetop < topleft && texturetop < topright) topleft = topright = texturetop;
			if (texturebottom > bottomleft && texturebottom > bottomright) bottomleft = bottomright = texturebottom;
		}
	}
	else
	{
		//
		//
		// if both sides of the line are in the same sector and the sector
		// doesn't have a Transfer_Heights special don't clip to the planes
		// Clipping to the planes is not necessary and can even produce
		// unwanted side effects.
		//
		//
		topleft = texturetop;
		topright = texturetop + skew;
		bottomleft = texturebottom;
		bottomright = texturebottom + skew;
	}

	// nothing visible - skip the rest
	if (topleft<=bottomleft && topright<=bottomright) return;


	//
	//
	// set up texture coordinate stuff
	//
	//
	float t_ofs = seg->sidedef->GetTextureXOffset(side_t::mid);

	if (texture)
	{
		// First adjust the texture offset so that the left edge of the linedef is inside the range [0..1].
		float texwidth = tci.TextureAdjustWidth();

		if (t_ofs >= 0)
		{
			float div = t_ofs / texwidth;
			t_ofs = (div - RoundDown(div)) * texwidth;
		}
		else
		{
			float div = (-t_ofs) / texwidth;
			t_ofs = texwidth - (div - RoundDown(div)) * texwidth;
		}


		// Now check whether the linedef is completely within the texture range of [0..1].
		// If so we should use horizontal texture clamping to prevent filtering artifacts
		// at the edges.

		float textureoffset = tci.TextureOffset(t_ofs);
		int righttex = int(textureoffset) + seg->sidedef->TexelLength;

		if ((textureoffset == 0 && righttex <= tci.mRenderWidth) ||
			(textureoffset >= 0 && righttex == tci.mRenderWidth))
		{
			flags |= HWF_CLAMPX;
		}
		else
		{
			flags &= ~HWF_CLAMPX;
		}
		if (!wrap)
		{
			flags |= HWF_CLAMPY;
		}
	}
	if (mirrory)
	{
		tci.mRenderHeight = -tci.mRenderHeight;
		tci.mScale.Y = -tci.mScale.Y;
		flags |= HWF_NOSLICE;
	}
	if (seg->linedef->isVisualPortal())
	{
		// mid textures on portal lines need the same offsetting as mid textures on sky lines
		flags |= HWF_SKYHACK;
	}
	SetWallCoordinates(seg, &tci, texturetop, topleft, topright, bottomleft, bottomright, t_ofs, skew);

	//
	//
	// draw fog sheet if required
	//
	//
	if (drawfogboundary)
	{
		flags |= HWF_NOSPLITUPPER|HWF_NOSPLITLOWER;
		type=RENDERWALL_FOGBOUNDARY;
		auto savetex = texture;
		texture = NULL;
		PutWall(di, true);
		if (!savetex)
		{
			flags &= ~(HWF_NOSPLITUPPER|HWF_NOSPLITLOWER);
			return;
		}
		texture = savetex;
		type=RENDERWALL_M2SNF;
	}
	else type=RENDERWALL_M2S;

	//
	//
	// set up alpha blending
	//
	//
	bool sideHasAlpha = seg->sidedef->HasAlpha();
	if (sideHasAlpha || seg->linedef->alpha != 0)
	{
		bool translucent = false;

		switch (seg->linedef->flags& ML_ADDTRANS)//TRANSBITS)
		{
		case 0:
			RenderStyle=STYLE_Translucent;
			alpha = sideHasAlpha ? seg->sidedef->alpha : seg->linedef->alpha;
			translucent =alpha < 1. || (texture && texture->GetTranslucency());
			break;

		case ML_ADDTRANS:
			RenderStyle=STYLE_Add;
			alpha = sideHasAlpha ? seg->sidedef->alpha : seg->linedef->alpha;
			translucent=true;
			break;
		}

		//
		//
		// for textures with large empty areas only the visible parts are drawn.
		// If these textures come too close to the camera they severely affect performance
		// if stacked closely together
		// Recognizing vertical gaps is rather simple and well worth the effort.
		//
		//
		FloatRect *splitrect;
		int v = texture->GetAreas(&splitrect);
		if (seg->frontsector == seg->backsector) flags |= HWF_NOSPLIT;	// we don't need to do vertex splits if a line has both sides in the same sector
		if (v>0 && !drawfogboundary && !(seg->linedef->flags&ML_WRAP_MIDTEX) && !(flags & HWF_NOSLICE) && skew == 0)
		{
			// split the poly!
			int i,t=0;
			float v_factor=(zbottom[0]-ztop[0])/(tcs[LOLFT].v-tcs[UPLFT].v);
			// only split the vertical area of the polygon that does not contain slopes.
			float splittopv = max(tcs[UPLFT].v, tcs[UPRGT].v);
			float splitbotv = min(tcs[LOLFT].v, tcs[LORGT].v);

			// this is split vertically into sections.
			for(i=0;i<v;i++)
			{
				// the current segment is below the bottom line of the splittable area
				// (IOW. the whole wall has been done)
				if (splitrect[i].top>=splitbotv) break;

				float splitbot=splitrect[i].top+splitrect[i].height;

				// the current segment is above the top line of the splittable area
				if (splitbot<=splittopv) continue;

				HWWall split = *this;

				// the top line of the current segment is inside the splittable area
				// use the splitrect's top as top of this segment
				// if not use the top of the remaining polygon
				if (splitrect[i].top>splittopv)
				{
					split.ztop[0]=split.ztop[1]= ztop[0]+v_factor*(splitrect[i].top-tcs[UPLFT].v);
					split.tcs[UPLFT].v=split.tcs[UPRGT].v=splitrect[i].top;
				}
				// the bottom line of the current segment is inside the splittable area
				// use the splitrect's bottom as bottom of this segment
				// if not use the bottom of the remaining polygon
				if (splitbot<=splitbotv)
				{
					split.zbottom[0]=split.zbottom[1]=ztop[0]+v_factor*(splitbot-tcs[UPLFT].v);
					split.tcs[LOLFT].v=split.tcs[LORGT].v=splitbot;
				}
				//
				//
				// Draw the stuff
				//
				//
				if (front->e->XFloor.lightlist.Size()==0 || di->isFullbrightScene()) split.PutWall(di, translucent);
				else split.SplitWall(di, front, translucent);

				t=1;
			}
			render_texsplit+=t;
		}
		else
		{
			//
			//
			// Draw the stuff without splitting
			//
			//
			if (front->e->XFloor.lightlist.Size()==0 || di->isFullbrightScene()) PutWall(di, translucent);
			else SplitWall(di, front, translucent);
		}
		alpha=1.0f;
	}
	// restore some values that have been altered in this function
	glseg=glsave;
	flags&=~(HWF_CLAMPX|HWF_CLAMPY|HWF_NOSPLITUPPER|HWF_NOSPLITLOWER);
	RenderStyle = STYLE_Normal;
}


//==========================================================================
//
//
//
//==========================================================================
void HWWall::BuildFFBlock(HWWallDispatcher *di, seg_t * seg, F3DFloor * rover, int roverIndex,
	float ff_topleft, float ff_topright,
	float ff_bottomleft, float ff_bottomright)
{
	side_t * mastersd = rover->master->sidedef[0];
	float to;
	lightlist_t * light;
	bool translucent;
	int savelight = lightlevel;
	FColormap savecolor = Colormap;
	float ul;
	float texlength;
	FTexCoordInfo tci;

	lightmap = nullptr;
	if (seg->sidedef == seg->linedef->sidedef[0])
		lightmap = seg->linedef->sidedef[1]->lightmap;
	else
		lightmap = seg->linedef->sidedef[0]->lightmap;

	if (lightmap)
		lightmap += 4 + roverIndex;

	if (rover->flags&FF_FOG)
	{
		if (!di->isFullbrightScene())
		{
			// this may not yet be done
			light = P_GetPlaneLight(rover->target, rover->top.plane, true);
			Colormap.Clear();
			Colormap.LightColor = light->extra_colormap.FadeColor;
			// the fog plane defines the light di->Level->, not the front sector
			lightlevel = RescaleLightLevel(*light->p_lightlevel);
			texture = NULL;
			type = RENDERWALL_FFBLOCK;
		}
		else return;
	}
	else
	{

		if (rover->flags&FF_UPPERTEXTURE)
		{
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::top), true);
			if (!texture || !texture->isValid()) return;
			GetTexCoordInfo(texture, &tci, seg->sidedef, side_t::top);
		}
		else if (rover->flags&FF_LOWERTEXTURE)
		{
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::bottom), true);
			if (!texture || !texture->isValid()) return;
			GetTexCoordInfo(texture, &tci, seg->sidedef, side_t::bottom);
		}
		else
		{
			texture = TexMan.GetGameTexture(mastersd->GetTexture(side_t::mid), true);
			if (!texture || !texture->isValid()) return;
			GetTexCoordInfo(texture, &tci, mastersd, side_t::mid);
		}

		to = (rover->flags&(FF_UPPERTEXTURE | FF_LOWERTEXTURE)) ? 0 : tci.TextureOffset(mastersd->GetTextureXOffset(side_t::mid));

		ul = tci.FloatToTexU(to + tci.TextureOffset(seg->sidedef->GetTextureXOffset(side_t::mid)));

		texlength = tci.FloatToTexU(seg->sidedef->TexelLength);

		tcs[UPLFT].u = tcs[LOLFT].u = ul + texlength * glseg.fracleft;
		tcs[UPRGT].u = tcs[LORGT].u = ul + texlength * glseg.fracright;

		float rowoffset = tci.RowOffset(seg->sidedef->GetTextureYOffset(side_t::mid));
		to = (rover->flags&(FF_UPPERTEXTURE | FF_LOWERTEXTURE)) ?
			0.f : tci.RowOffset(mastersd->GetTextureYOffset(side_t::mid));

		to += rowoffset + rover->top.model->GetPlaneTexZ(rover->top.isceiling);

		tcs[UPLFT].v = tci.FloatToTexV(to - ff_topleft);
		tcs[UPRGT].v = tci.FloatToTexV(to - ff_topright);
		tcs[LOLFT].v = tci.FloatToTexV(to - ff_bottomleft);
		tcs[LORGT].v = tci.FloatToTexV(to - ff_bottomright);
		type = RENDERWALL_FFBLOCK;
		CheckTexturePosition(&tci);

		texcoord* srclightuv;
		if (lightmap && lightmap->Type != ST_NULL)
		{
			srclightuv = (texcoord*)lightmap->TexCoords;
			lindex = (float)lightmap->LightmapNum;
		}
		else
		{
			srclightuv = (texcoord*)ZeroLightmapUVs;
			lindex = -1.0f;
		}

		lightuv[UPLFT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracleft;
		lightuv[LOLFT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracleft;
		lightuv[UPRGT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracright;
		lightuv[LORGT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracright;

		lightuv[UPLFT].v = srclightuv[UPLFT].v;
		lightuv[UPRGT].v = srclightuv[UPRGT].v;
		lightuv[LOLFT].v = srclightuv[LOLFT].v;
		lightuv[LORGT].v = srclightuv[LORGT].v;
	}

	ztop[0] = ff_topleft;
	ztop[1] = ff_topright;
	zbottom[0] = ff_bottomleft;//-0.001f;
	zbottom[1] = ff_bottomright;

	if (rover->flags&(FF_TRANSLUCENT | FF_ADDITIVETRANS | FF_FOG))
	{
		alpha = rover->alpha / 255.0f;
		RenderStyle = (rover->flags&FF_ADDITIVETRANS) ? STYLE_Add : STYLE_Translucent;
		translucent = true;
		type = texture ? RENDERWALL_M2S : RENDERWALL_COLOR;
	}
	else
	{
		alpha = 1.0f;
		RenderStyle = STYLE_Normal;
		translucent = false;
	}

	sector_t * sec = sub ? sub->sector : seg->frontsector;

	if (sec->e->XFloor.lightlist.Size() == 0 || di->isFullbrightScene()) PutWall(di, translucent);
	else SplitWall(di, sec, translucent);

	alpha = 1.0f;
	lightlevel = savelight;
	Colormap = savecolor;
	flags &= ~HWF_CLAMPY;
	RenderStyle = STYLE_Normal;
}


//==========================================================================
//
//
//
//==========================================================================

__forceinline void HWWall::GetPlanePos(F3DFloor::planeref *planeref, float &left, float &right)
{
	left=planeref->plane->ZatPoint(vertexes[0]);
	right=planeref->plane->ZatPoint(vertexes[1]);
}

//==========================================================================
//
//
//
//==========================================================================
void HWWall::InverseFloors(HWWallDispatcher *di, seg_t * seg, sector_t * frontsector,
	float topleft, float topright,
	float bottomleft, float bottomright)
{
	TArray<F3DFloor *> & frontffloors = frontsector->e->XFloor.ffloors;

	for (unsigned int i = 0; i < frontffloors.Size(); i++)
	{
		F3DFloor * rover = frontffloors[i];
		if (rover->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
		if (!(rover->flags&FF_EXISTS)) continue;
		if (!(rover->flags&FF_RENDERSIDES)) continue;
		if (!(rover->flags&(FF_INVERTSIDES | FF_ALLSIDES))) continue;

		float ff_topleft;
		float ff_topright;
		float ff_bottomleft;
		float ff_bottomright;

		GetPlanePos(&rover->top, ff_topleft, ff_topright);
		GetPlanePos(&rover->bottom, ff_bottomleft, ff_bottomright);

		// above ceiling
		if (ff_bottomleft > topleft && ff_bottomright > topright) continue;

		if (ff_topleft > topleft && ff_topright > topright)
		{
			// the new section overlaps with the previous one - clip it!
			ff_topleft = topleft;
			ff_topright = topright;
		}
		if (ff_bottomleft < bottomleft && ff_bottomright < bottomright)
		{
			ff_bottomleft = bottomleft;
			ff_bottomright = bottomright;
		}
		if (ff_topleft < ff_bottomleft || ff_topright < ff_bottomright) continue;

		BuildFFBlock(di, seg, rover, i, ff_topleft, ff_topright, ff_bottomleft, ff_bottomright);
		topleft = ff_bottomleft;
		topright = ff_bottomright;

		if (topleft <= bottomleft && topright <= bottomright) return;
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWWall::ClipFFloors(HWWallDispatcher *di, seg_t * seg, F3DFloor * ffloor, int ffloorIndex, sector_t * frontsector,
	float topleft, float topright,
	float bottomleft, float bottomright)
{
	TArray<F3DFloor *> & frontffloors = frontsector->e->XFloor.ffloors;

	const unsigned int flags = ffloor->flags & (FF_SWIMMABLE | FF_TRANSLUCENT);

	for (unsigned int i = 0; i < frontffloors.Size(); i++)
	{
		F3DFloor * rover = frontffloors[i];
		if (rover->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
		if (!(rover->flags&FF_EXISTS)) continue;
		if (!(rover->flags&FF_RENDERSIDES)) continue;
		if ((rover->flags&(FF_SWIMMABLE | FF_TRANSLUCENT)) != flags) continue;

		float ff_topleft;
		float ff_topright;
		float ff_bottomleft;
		float ff_bottomright;

		GetPlanePos(&rover->top, ff_topleft, ff_topright);

		// we are completely below the bottom so unless there are some
		// (unsupported) intersections there won't be any more floors that
		// could clip this one.
		if (ff_topleft < bottomleft && ff_topright < bottomright) goto done;

		GetPlanePos(&rover->bottom, ff_bottomleft, ff_bottomright);
		// above top line?
		if (ff_bottomleft > topleft && ff_bottomright > topright) continue;

		// overlapping the top line
		if (ff_topleft >= topleft && ff_topright >= topright)
		{
			// overlapping with the entire range
			if (ff_bottomleft <= bottomleft && ff_bottomright <= bottomright) return;
			else if (ff_bottomleft > bottomleft && ff_bottomright > bottomright)
			{
				topleft = ff_bottomleft;
				topright = ff_bottomright;
			}
			else
			{
				// an intersecting case.
				// proper handling requires splitting but
				// I don't need this right now.
			}
		}
		else if (ff_topleft <= topleft && ff_topright <= topright)
		{
			BuildFFBlock(di, seg, ffloor, ffloorIndex, topleft, topright, ff_topleft, ff_topright);
			if (ff_bottomleft <= bottomleft && ff_bottomright <= bottomright) return;
			topleft = ff_bottomleft;
			topright = ff_bottomright;
		}
		else
		{
			// an intersecting case.
			// proper handling requires splitting but
			// I don't need this right now.
		}
	}

done:
	// if the program reaches here there is one block left to draw
	BuildFFBlock(di, seg, ffloor, ffloorIndex, topleft, topright, bottomleft, bottomright);
}

//==========================================================================
//
//
//
//==========================================================================
void HWWall::DoFFloorBlocks(HWWallDispatcher *di, seg_t * seg, sector_t * frontsector, sector_t * backsector,
	float fch1, float fch2, float ffh1, float ffh2,
	float bch1, float bch2, float bfh1, float bfh2)

{
	TArray<F3DFloor *> & backffloors = backsector->e->XFloor.ffloors;
	float topleft, topright, bottomleft, bottomright;
	bool renderedsomething = false;

	// if the ceilings intersect use the backsector's height because this sector's ceiling will
	// obstruct the redundant parts.

	if (fch1 < bch1 && fch2 < bch2)
	{
		topleft = fch1;
		topright = fch2;
	}
	else
	{
		topleft = bch1;
		topright = bch2;
	}

	if (ffh1 > bfh1 && ffh2 > bfh2)
	{
		bottomleft = ffh1;
		bottomright = ffh2;
	}
	else
	{
		bottomleft = bfh1;
		bottomright = bfh2;
	}

	for (unsigned int i = 0; i < backffloors.Size(); i++)
	{
		F3DFloor * rover = backffloors[i];
		if (rover->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
		if (!(rover->flags&FF_EXISTS)) continue;
		if (!(rover->flags&FF_RENDERSIDES) || (rover->flags&FF_INVERTSIDES)) continue;

		float ff_topleft;
		float ff_topright;
		float ff_bottomleft;
		float ff_bottomright;

		GetPlanePos(&rover->top, ff_topleft, ff_topright);
		GetPlanePos(&rover->bottom, ff_bottomleft, ff_bottomright);

		// completely below floor
		if (ff_topleft <= bottomleft && ff_topright <= bottomright) continue;

		// completely above ceiling
		if (ff_bottomleft > topleft && ff_bottomright > topright && !renderedsomething) continue;

		if (ff_topleft > topleft && ff_topright > topright)
		{
			// the new section overlaps with the previous one - clip it!
			ff_topleft = topleft;
			ff_topright = topright;
		}

		// do all inverse floors above the current one it there is a gap between the
		// last 3D floor and this one.
		if (topleft > ff_topleft && topright > ff_topright)
			InverseFloors(di, seg, frontsector, topleft, topright, ff_topleft, ff_topright);

		// if translucent or liquid clip away adjoining parts of the same type of FFloors on the other side
		if (rover->flags&(FF_SWIMMABLE | FF_TRANSLUCENT))
			ClipFFloors(di, seg, rover, i, frontsector, ff_topleft, ff_topright, ff_bottomleft, ff_bottomright);
		else
			BuildFFBlock(di, seg, rover, i, ff_topleft, ff_topright, ff_bottomleft, ff_bottomright);

		topleft = ff_bottomleft;
		topright = ff_bottomright;
		renderedsomething = true;
		if (topleft <= bottomleft && topright <= bottomright) return;
	}

	// draw all inverse floors below the lowest one
	if (frontsector->e->XFloor.ffloors.Size() > 0)
	{
		if (topleft > bottomleft || topright > bottomright)
			InverseFloors(di, seg, frontsector, topleft, topright, bottomleft, bottomright);
	}
}

CVAR(Int, topskew, 0, 0)
CVAR(Int, midskew, 0, 0)
CVAR(Int, bottomskew, 0, 0)

#include "hw_retainedworld.h"

static void CapturePreparedWall(HWWallDispatcher* di, HWWall* wall)
{
	if (!di->PreparedCapture || di->SuppressSubmit || wall->Prepared) return;
	auto& geometry = RetainedWorld::Geometry.emplace_back();
	geometry.Normal = wall->glseg.Normal();
	auto ptr = geometry.Vertices.data();
	if (wall->CreateVertices(ptr, false) != 4)
	{
		I_FatalError("Retained ordinary wall unexpectedly split");
	}
	wall->Prepared = &geometry;
}

bool CanOwnOpaqueWalls(HWDrawInfo* di)
{
	return vk_drawownership && vk_retainedworld == 2 && !vk_retainedworld_verify && gl_texture && !gl_seamless &&
		!r_skipmats && screen->IsVulkan() && screen->BuffersArePersistent() &&
		!di->outer && !di->mCurrentPortal && !di->mClipPortal && !di->Viewpoint.bDoOob && !di->Viewpoint.bDoOrtho &&
		!di->Level->HasDynamicLights && !sysCallbacks.PreBindTexture && RetainedWorldShadersCompatible();
}

static bool SelectOwnedWall(HWWallDispatcher* di, HWWall& source, seg_t* seg, sector_t* front,
	sector_t* back, RetainedWorld::Entry& entry, const RetainedWorld::SideInputs& inputs)
{
	HWSceneScopeTimer timer(GetSceneSurfaceProfile().OwnedSelectionNS);
	const bool changed = RetainedWorld::CompactSectors ?
		RetainedWorld::UpdateEntryAppearanceStorage<true>(di, front, seg, seg->Subsector, entry, &inputs) :
		RetainedWorld::UpdateEntryAppearanceStorage<false>(di, front, seg, seg->Subsector, entry, &inputs);
	if (entry.Output.upper.Size() || entry.Output.lower.Size()) return false;
	for (const auto& wall : entry.Output.list)
		if (!wall.CanDrawPersistentWall()) return false;
	if (!di->OwnedLightingReady)
	{
		di->OwnedLightingContext = RetainedWorld::ValidateLightingContext(di->Level);
		di->OwnedExtraLight = getExtraLight();
		di->OwnedLightingReady = true;
	}
	if (!entry.OwnerVersion || changed || entry.OwnedLightingContext != di->OwnedLightingContext ||
		entry.OwnedLightMode != di->lightmode || entry.OwnedExtraLight != di->OwnedExtraLight)
	{
		// [Karto] Never recycle certificates when slots or level resources are reused.
		if (RetainedWorld::NextOwnerVersion == UINT64_MAX) return false;
		entry.OwnerVersion = ++RetainedWorld::NextOwnerVersion;
		entry.OwnedLightingContext = di->OwnedLightingContext;
		entry.OwnedLightMode = di->lightmode;
		entry.OwnedExtraLight = di->OwnedExtraLight;
	}
	// [Karto] Run sky registration in this original wall job before selecting its ordinary outputs, preserving portal encounter order.
	RetainedWorld::ReplayViewEffects(di, source, seg, front, back);
	for (auto& wall : entry.Output.list)
	{
		di->di->drawlists[GLDL_PLAINWALLS].OwnedWalls.Push({ &wall, entry.OwnerVersion, di->SubmissionOrder, di->SubmissionPart++ });
		if (SceneProfiling)
		{
			++GetSceneSurfaceProfile().Submitted;
			++GetSceneSurfaceProfile().OpaqueRegular;
		}
	}
	if (SceneProfiling)
	{
		auto& profile = GetSceneSurfaceProfile();
		if (entry.Output.list.Size()) ++profile.OwnedSelections;
		else ++profile.OwnedEmptySelections;
		if (front->GetTexture(0) == skyflatnum || front->GetTexture(1) == skyflatnum ||
			(back && (back->GetTexture(0) == skyflatnum || back->GetTexture(1) == skyflatnum)))
		{
			++profile.OwnedSkySelections;
			profile.OwnedSkyRows += entry.Output.list.Size();
		}
	}
	return true;
}

struct FGpuPersistentWallInputs
{
	FGameTexture* TextureTop = nullptr;
	FGameTexture* TextureMid = nullptr;
	FGameTexture* TextureBottom = nullptr;
	std::array<float, 44> Values = {};
	std::array<int, 12> Modes = {};
	bool operator==(const FGpuPersistentWallInputs& other) const
	{
		return TextureTop == other.TextureTop && TextureMid == other.TextureMid && TextureBottom == other.TextureBottom &&
			Modes == other.Modes && memcmp(Values.data(), other.Values.data(), sizeof(Values)) == 0;
	}
};

struct FGpuResidentAppearanceInputs
{
	uint64_t LightingContext = 0;
	uint64_t WallContext = 0;
	std::array<int, 7> Modes = {};
	FColormap Colormap = {};
	std::array<uint32_t, 4> Colors = {};
	TextureManipulation TextureFx = {};
	std::array<float, 3> Material = {};
	bool operator==(const FGpuResidentAppearanceInputs& other) const
	{
		return LightingContext == other.LightingContext && WallContext == other.WallContext && Modes == other.Modes &&
			memcmp(&Colormap, &other.Colormap, sizeof(Colormap)) == 0 && Colors == other.Colors &&
			memcmp(&TextureFx, &other.TextureFx, sizeof(TextureFx)) == 0 && Material == other.Material;
	}
};
static uint64_t GpuResidentNextRevision = 0;

// [Karto] A whole-seg bypass is safe only when every output fits the bounded opaque owner; partial capture must fall back.
struct FGpuPersistentPart
{
	int Type = RENDERWALL_M1S;
	HWWall Wall = {};
	std::array<FFlatVertex, 4> Vertices = {};
	uint32_t Row = UINT32_MAX;
	uint32_t TextureId = 0;
	uint32_t SectorIndex = 0;
	int32_t SideIndex = -1;
	FGpuStaticMaterial StaticMaterial = {};
	FGpuResidentAppearanceInputs Appearance;
	bool AppearanceValid = false;
};

struct FGpuPersistentWall
{
	FGpuPersistentWallInputs Inputs;
	std::array<FGpuPersistentPart, 3> Parts = {};
	int PartCount = 0;
	uint64_t CheckedEpoch = UINT64_MAX;
	uint64_t BorrowedView = 0;
	bool Valid = false;
	bool CaptureRejected = false;
};

static std::vector<int32_t> GpuPersistentSegOwner;
static std::vector<FGpuPersistentWall> GpuPersistentOwners;
static FLevelLocals* GpuPersistentOwnersLevel = nullptr;

void GPUClearPersistentWalls()
{
	// [Karto] Map storage can reuse seg addresses/indices, so old owners must disappear before new bake identities are created.
	GpuPersistentSegOwner.clear();
	GpuPersistentOwners.clear();
	GpuPersistentOwnersLevel = nullptr;
}

static void CaptureGpuOwnerStaticMaterial(HWWall* wall, FRenderState& state)
{
	if (wall->GpuOwnerSlot < 0 || wall->GpuOwnerSlot >= (int)GpuPersistentOwners.size()) return;
	auto& owner = GpuPersistentOwners[wall->GpuOwnerSlot];
	for (int p = 0; p < owner.PartCount; ++p)
	{
		if (owner.Parts[p].Type == wall->type)
		{
			state.CaptureGpuStaticMaterial(owner.Parts[p].StaticMaterial);
			return;
		}
	}
}

static void RejectGpuPersistentWall(HWWall* wall)
{
	if (wall->GpuOwnerCandidate)
	{
		wall->GpuOwnerCandidate->CaptureRejected = true;
		if (SceneProfiling) ++GetSceneSurfaceProfile().GpuOwnerCaptureRejected;
	}
}

static void CaptureGpuPersistentWall(HWWall* wall)
{
	HWSceneScopeTimer captureTimer(GetSceneSurfaceProfile().GpuOwnerCaptureNS);
	auto owner = wall->GpuOwnerCandidate;
	if (!owner) return;
	if (!wall->CanGpuDriveWall(true) || owner->PartCount >= (int)owner->Parts.size())
	{
		// [Karto] Ignoring one unsupported part would make a warm whole-seg replay omit live output.
		owner->CaptureRejected = true;
		if (SceneProfiling) ++GetSceneSurfaceProfile().GpuOwnerCaptureRejected;
		return;
	}
	auto& part = owner->Parts[owner->PartCount];
	part.Type = wall->type;
	part.TextureId = wall->texture ? uint32_t(wall->texture->GetID().GetIndex()) : 0;
	part.SectorIndex = uint32_t((wall->sub ? wall->sub->sector : wall->frontsector)->Index());
	part.SideIndex = (wall->seg && wall->seg->sidedef) ? wall->seg->sidedef->Index() : -1;
	part.Wall = *wall;
	part.Wall.GpuOwnerCandidate = nullptr;
	part.Wall.GpuAppearanceId = UINT32_MAX;
	part.Wall.GpuAppearanceRevision = ++GpuResidentNextRevision;
	part.AppearanceValid = false;
	part.Wall.GpuResidentEpoch = 0;
	part.Wall.GpuResidentRow = UINT32_MAX;
	part.Wall.vertcount = 0;
	part.Wall.vertindex = 0;
	auto cursor = part.Vertices.data();
	if (wall->CreateVertices(cursor, false) != 4)
	{
		owner->CaptureRejected = true;
		if (SceneProfiling) ++GetSceneSurfaceProfile().GpuOwnerCaptureRejected;
		return;
	}
	part.Row = UINT32_MAX;
	++owner->PartCount;
	owner->CheckedEpoch = UINT64_MAX;
	if (SceneProfiling) ++GetSceneSurfaceProfile().GpuPersistentRebuilds;
}

static bool TryGpuPersistentWall(HWWall& wall, HWWallDispatcher* di, seg_t* seg, sector_t* front, sector_t* back, bool culled)
{
	HWSceneScopeTimer admissionTimer(GetSceneSurfaceProfile().GpuOwnerAdmissionNS);
	auto reject = [](EGpuOwnerRejectReason reason)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().GpuOwnerRejects[reason];
		return false;
	};
	if (!vk_gpu_driven_persistent || !vk_gpu_driven_bake || !vk_gpu_driven_candidate || !vk_gpu_driven_geometry ||
		!vk_gpu_driven_indexed || !vk_gpu_driven_bindless || vk_retainedworld || gl_wallcache || gl_seamless ||
		(vk_gpu_driven_resident && vk_sharedappearance) ||
		!di->di || di->SuppressSubmit || di->PreparedCapture || di->PreparedReplay || !screen->IsVulkan() || !screen->BuffersArePersistent()) return reject(GOR_Unavailable);
	if (SceneProfiling) ++GetSceneSurfaceProfile().GpuPersistentRequests;
	if (culled || !seg || !seg->linedef || !seg->sidedef || !front ||
		front != seg->frontsector || !wall.sub || wall.sub->sector != front || seg->sidedef->linedef != seg->linedef ||
		back != seg->backsector) return reject(GOR_Identity);
	if (di->di->outer || di->di->mCurrentPortal || di->di->mClipPortal || di->di->Viewpoint.bDoOob || di->di->Viewpoint.bDoOrtho) return reject(GOR_PortalView);
	if (di->Level->HasDynamicLights || sysCallbacks.PreBindTexture) return reject(GOR_DynamicLight);
	if (front->floorplane.isSlope() || front->ceilingplane.isSlope() ||
		(back && (back->floorplane.isSlope() || back->ceilingplane.isSlope())) ||
		front->GetHeightSec() || (back && back->GetHeightSec()) ||
		front->e->XFloor.ffloors.Size() || front->e->XFloor.lightlist.Size() ||
		(back && (back->e->XFloor.ffloors.Size() || back->e->XFloor.lightlist.Size())) ||
		seg->sidedef->AttachedDecals || seg->sidedef->lightmap ||
		(seg->sidedef->Flags & (WALLF_POLYOBJ | WALLF_BLOCKRENDERING | WALLF_DITHERTRANS_TOP | WALLF_DITHERTRANS_BOTTOM | WALLF_DITHERTRANS_MID)) ||
		seg->linedef->isVisualPortal() || seg->linedef->GetTransferredPortal() || seg->linedef->special == Line_Horizon || seg->linedef->special == Line_Mirror) return reject(GOR_SlopeOrSpecial);
	// [Karto] Admit sky segs (ordinary parts can be owned), but never take the valid-empty bypass for a sky-adjacent owner.
	// Portal-emitting segs fall back via PutPortal's reject.
	const bool frontSky = front->GetTexture(sector_t::floor) == skyflatnum || front->GetTexture(sector_t::ceiling) == skyflatnum;
	const bool backSky = back && (back->GetTexture(sector_t::floor) == skyflatnum || back->GetTexture(sector_t::ceiling) == skyflatnum);
	if (frontSky && SceneProfiling) ++GetSceneSurfaceProfile().GpuOwnerRejects[backSky ? GOR_Sky : GOR_SkyFront];
	else if (backSky && SceneProfiling) ++GetSceneSurfaceProfile().GpuOwnerRejects[GOR_SkyBack];
	auto planeEffects = [](sector_t* sector)
	{
		return sector && (sector->ValidatePortal(sector_t::floor) || sector->ValidatePortal(sector_t::ceiling) ||
			sector->GetReflect(sector_t::floor) > 0 || sector->GetReflect(sector_t::ceiling) > 0);
	};
	if (planeEffects(front) || planeEffects(back)) return reject(GOR_PlaneEffect);
	auto side = seg->sidedef;
	auto textureTop = TexMan.GetGameTexture(side->GetTexture(side_t::top), true);
	auto textureMid = TexMan.GetGameTexture(side->GetTexture(side_t::mid), true);
	auto textureBottom = TexMan.GetGameTexture(side->GetTexture(side_t::bottom), true);
	auto ordinaryTexture = [](FGameTexture* texture)
	{
		return !texture || !texture->isValid() || (texture->GetShaderIndex() == 0 && !texture->isHardwareCanvas() &&
			!texture->isFullbright() && !texture->isMasked() && !texture->GetTranslucency());
	};
	if (back ? ((textureMid && textureMid->isValid()) || !ordinaryTexture(textureTop) || !ordinaryTexture(textureBottom) ||
		(!di->isFullbrightScene() && CheckFog(di->Level, front, back, di->lightmode))) : !ordinaryTexture(textureMid)) return reject(GOR_Texture);
	float topGlow[4], bottomGlow[4];
	if (front->GetWallGlow(topGlow, bottomGlow)) return reject(GOR_WallGlow);
	const bool forward = side == seg->linedef->sidedef[0];
	auto v1 = forward ? seg->linedef->v1 : seg->linedef->v2;
	auto v2 = forward ? seg->linedef->v2 : seg->linedef->v1;
	const float ffh1 = front->floorplane.ZatPoint(v1);
	const float ffh2 = front->floorplane.ZatPoint(v2);
	const float fch1 = front->ceilingplane.ZatPoint(v1);
	const float fch2 = front->ceilingplane.ZatPoint(v2);
	if (fch1 <= ffh1 && fch2 <= ffh2) return reject(GOR_Degenerate);
	auto texelW = [](FGameTexture* t) { return t ? float(t->GetTexelWidth()) : 0.f; };
	auto texelH = [](FGameTexture* t) { return t ? float(t->GetTexelHeight()) : 0.f; };
	FGpuPersistentWallInputs inputs;
	inputs.TextureTop = textureTop;
	inputs.TextureMid = textureMid;
	inputs.TextureBottom = textureBottom;
	inputs.Values = { float(v1->fX()), float(v2->fX()), float(v1->fY()), float(v2->fY()),
		ffh1, fch1, back ? float(back->floorplane.ZatPoint(v1)) : ffh1, back ? float(back->ceilingplane.ZatPoint(v1)) : fch1,
		float(front->GetPlaneTexZ(sector_t::floor)), float(front->GetPlaneTexZ(sector_t::ceiling)),
		back ? float(back->GetPlaneTexZ(sector_t::floor)) : 0.f, back ? float(back->GetPlaneTexZ(sector_t::ceiling)) : 0.f,
		float(side->GetTextureXOffset(side_t::top)), float(side->GetTextureYOffset(side_t::top)),
		float(side->GetTextureXScale(side_t::top)), float(side->GetTextureYScale(side_t::top)),
		float(side->GetTextureXOffset(side_t::mid)), float(side->GetTextureYOffset(side_t::mid)),
		float(side->GetTextureXScale(side_t::mid)), float(side->GetTextureYScale(side_t::mid)),
		float(side->GetTextureXOffset(side_t::bottom)), float(side->GetTextureYOffset(side_t::bottom)),
		float(side->GetTextureXScale(side_t::bottom)), float(side->GetTextureYScale(side_t::bottom)),
		texelW(textureTop), texelH(textureTop), texelW(textureMid), texelH(textureMid), texelW(textureBottom), texelH(textureBottom),
		ffh2, fch2,
		textureTop ? textureTop->GetScaleX() : 0.f, textureTop ? textureTop->GetScaleY() : 0.f,
		textureTop ? textureTop->GetDisplayWidth() : 0.f, textureTop ? textureTop->GetDisplayHeight() : 0.f,
		textureMid ? textureMid->GetScaleX() : 0.f, textureMid ? textureMid->GetScaleY() : 0.f,
		textureMid ? textureMid->GetDisplayWidth() : 0.f, textureMid ? textureMid->GetDisplayHeight() : 0.f,
		textureBottom ? textureBottom->GetScaleX() : 0.f, textureBottom ? textureBottom->GetScaleY() : 0.f,
		textureBottom ? textureBottom->GetDisplayWidth() : 0.f, textureBottom ? textureBottom->GetDisplayHeight() : 0.f };
	// [Karto] Plane texture writes can create sky encounters without moving geometry, so they invalidate the whole owner too.
	const bool forceWorldPanning = di->Level->flags3 & LEVEL3_FORCEWORLDPANNING;
	inputs.Modes = { int(seg->linedef->flags & (ML_DONTPEGBOTTOM | ML_DONTPEGTOP | ML_DRAWFULLHEIGHT | ML_TWOSIDED | ML_3DMIDTEX)),
		int(side->TexelLength), front->special,
		int((textureTop && textureTop->useWorldPanning()) || forceWorldPanning),
		int((textureMid && textureMid->useWorldPanning()) || forceWorldPanning),
		int((textureBottom && textureBottom->useWorldPanning()) || forceWorldPanning),
		int(front->transdoor), int(back && back->transdoor),
		front->GetTexture(sector_t::floor).GetIndex(), front->GetTexture(sector_t::ceiling).GetIndex(),
		back ? back->GetTexture(sector_t::floor).GetIndex() : -1, back ? back->GetTexture(sector_t::ceiling).GetIndex() : -1 };
	if (GpuPersistentOwnersLevel != di->Level)
	{
		GpuPersistentSegOwner.assign(di->Level->segs.Size(), -1);
		GpuPersistentOwners.clear();
		// [Karto] Draw lists borrow owner part addresses until the view finishes; pool growth must never relocate them.
		GpuPersistentOwners.reserve(32768);
		GpuPersistentOwnersLevel = di->Level;
	}
	const uint32_t segIndex = uint32_t(seg->Index());
	if (segIndex >= GpuPersistentSegOwner.size()) return reject(GOR_LevelMismatch);
	int32_t slot = GpuPersistentSegOwner[segIndex];
	if (slot < 0)
	{
		if (GpuPersistentOwners.size() >= 32768) return reject(GOR_Other);
		slot = int32_t(GpuPersistentOwners.size());
		GpuPersistentOwners.emplace_back();
		GpuPersistentSegOwner[segIndex] = slot;
	}
	auto& owner = GpuPersistentOwners[slot];
	if (!owner.Valid || !(owner.Inputs == inputs))
	{
		owner.Inputs = inputs;
		owner.Valid = false;
		owner.PartCount = 0;
		owner.CaptureRejected = false;
		wall.GpuOwnerCandidate = &owner;
		return false;
	}
	if (owner.PartCount == 0)
	{
		// [Karto] A zero-part capture is a valid-empty certificate only when the seg is not sky-adjacent, which can emit unregistered sky output with no ordinary part.
		if (frontSky || backSky) return false;
		if (SceneProfiling) ++GetSceneSurfaceProfile().GpuPersistentEmpty;
		static bool emptyLogged = false;
		if (!emptyLogged)
		{
			emptyLogged = true;
			Printf("G1PERSISTEMPTY valid_empty=1\n");
		}
		return true;
	}
	const auto epoch = GPUBakeEpoch();
	if (owner.CheckedEpoch != epoch)
	{
		owner.CheckedEpoch = epoch;
		for (int p = 0; p < owner.PartCount; ++p) owner.Parts[p].Row = UINT32_MAX;
	}
	// [Karto] A second encounter must not mutate a template already borrowed by this view or overwrite its referenced row.
	if (vk_gpu_driven_resident && owner.BorrowedView == RetainedWorld::View) return false;
	owner.BorrowedView = RetainedWorld::View;
	auto sub = wall.sub;
	HWSceneScopeTimer replayTimer(GetSceneSurfaceProfile().GpuOwnerReplayNS);
	for (int p = 0; p < owner.PartCount; ++p)
	{
		auto& part = owner.Parts[p];
		auto& output = vk_gpu_driven_resident ? part.Wall : wall;
		if (!vk_gpu_driven_resident) wall = part.Wall;
		output.sub = sub;
		output.seg = seg;
		output.frontsector = front;
		output.backsector = back;
		output.vertexes[0] = v1;
		output.vertexes[1] = v2;
		output.Colormap = front->Colormap;
		if (di->isFullbrightScene()) output.Colormap.Clear();
		output.vertcount = 0;
		output.vertindex = 0;
		output.dynlightindex = -1;
		const int texpos = part.Type == RENDERWALL_TOP ? side_t::top : part.Type == RENDERWALL_BOTTOM ? side_t::bottom : side_t::mid;
		int rel = 0;
		const int originalLight = hw_ClampLight(front->lightlevel);
		const bool foggy = !output.Colormap.FadeColor.isBlack() || (di->Level->flags & LEVEL_HASFADETABLE);
		output.lightlevel = RescaleLightLevel(side->GetLightLevel(foggy, originalLight, texpos, false, &rel));
		output.rellight = CalcRelLight(output.lightlevel, originalLight, rel);
		output.GpuResidentEpoch = 0;
		output.GpuResidentRow = UINT32_MAX;
		output.GpuOwnerSlot = slot;
		if (part.Row == UINT32_MAX && epoch)
		{
			uint32_t offset, count;
			GPUBakeLookupWall(seg, part.Type, part.Vertices.data(), &offset, &count, &part.Row);
		}
		if (part.Row != UINT32_MAX && epoch)
		{
			output.GpuResidentRow = part.Row;
			output.GpuResidentEpoch = epoch;
			if (SceneProfiling) ++GetSceneSurfaceProfile().GpuPersistentResident;
			static bool residentLogged = false;
			if (!residentLogged)
			{
				residentLogged = true;
				Printf("G1PERSISTRESIDENT row=%u epoch=%llu\n", part.Row, static_cast<unsigned long long>(epoch));
			}
		}
		if (vk_gpu_driven_resident)
		{
			FGpuResidentAppearanceInputs appearance;
			appearance.LightingContext = RetainedWorld::ValidateLightingContext(di->Level);
			appearance.WallContext = RetainedWorld::ValidateWallLightingContext(di->Level);
			appearance.Modes = { int(di->lightmode), output.lightlevel, output.rellight, getExtraLight(),
				int(di->isFullbrightScene()), int(side->Flags & WALLF_EXTCOLOR), int(side->textures[texpos].flags) };
			appearance.Colormap = output.Colormap;
			if (side->Flags & WALLF_EXTCOLOR)
			{
				appearance.Colors = { side->GetSpecialColor(texpos, side_t::walltop, front).d,
					side->GetSpecialColor(texpos, side_t::wallbottom, front).d, side->GetAdditiveColor(texpos, front).d, 0 };
				appearance.TextureFx = side->textures[texpos].TextureFx;
			}
			appearance.Material = { output.texture->GetGlossiness(), output.texture->GetSpecularLevel(), float(output.texture->GetShaderSpeed()) };
			if (!part.AppearanceValid || !(part.Appearance == appearance))
			{
				part.Appearance = appearance;
				part.AppearanceValid = true;
				output.GpuAppearanceRevision = ++GpuResidentNextRevision;
			}
			output.GpuAppearanceId = uint32_t(slot) * 3 + uint32_t(p);
			output.texture->setSeen();
			di->di->drawlists[GLDL_PLAINWALLS].NewWall(&output);
			if (SceneProfiling)
			{
				auto& profile = GetSceneSurfaceProfile();
				++profile.Submitted; ++profile.OpaqueRegular;
				if (part.Type == RENDERWALL_TOP) ++profile.OutOrdinaryTop;
				else if (part.Type == RENDERWALL_BOTTOM) ++profile.OutOrdinaryBottom;
				else ++profile.OutOrdinaryM1S;
			}
		}
		else output.PutWall(di, false);
	}
	if (SceneProfiling)
	{
		++GetSceneSurfaceProfile().GpuPersistentHits;
		if (owner.PartCount >= 2) ++GetSceneSurfaceProfile().GpuOwnerMultiPartCalls;
	}
	static bool logged[2] = {};
	const int sided = back ? 1 : 0;
	if (!logged[sided])
	{
		logged[sided] = true;
		Printf("G1PERSISTUSE early=2 two_sided=%d parts=%d owner_bytes=%llu\n", sided, owner.PartCount, static_cast<unsigned long long>(sizeof(FGpuPersistentWall)));
	}
	return true;
}

void HWWall::Process(HWWallDispatcher* di, seg_t* seg, sector_t* front, sector_t* back, bool culled)
{
	if (SceneProfiling && !di->SuppressSubmit)
	{
		auto &profile = GetSceneSurfaceProfile();
		++profile.ProcessCalls;
		if (!back) ++profile.OneSided;
		else ++profile.ProcessTwoSidedCalls;
		if (seg->sidedef->Flags & WALLF_POLYOBJ) ++profile.Polyobjects;
		if (seg->linedef->isVisualPortal()) ++profile.VisualPortals;
		const auto &realFront = di->Level->sectors[front->sectornum];
		const auto *realBack = back ? &di->Level->sectors[back->sectornum] : nullptr;
		if (front != &realFront || back != realBack) ++profile.FakeSectors;
		if (front->floorplane.isSlope() || front->ceilingplane.isSlope() ||
			(back && (back->floorplane.isSlope() || back->ceilingplane.isSlope())))
		{
			++profile.Slopes;
		}
		if (front->e->XFloor.ffloors.Size() || (back && back->e->XFloor.ffloors.Size()))
		{
			++profile.ThreeDFloors;
		}
		if (front->GetHeightSec() || (back && back->GetHeightSec())) ++profile.HeightSectors;
	}
	struct FProcessOutputCensus
	{
		HWSceneSurfaceProfile* Profile;
		seg_t* Seg;
		sector_t* Front;
		sector_t* Back;
		cycle_t Timer;
		uint64_t BeforeSubmitted;
		uint64_t BeforeOrdinaryOpaque;
		uint64_t BeforeTop;
		uint64_t BeforeBottom;
		uint64_t BeforeM2S;
		FProcessOutputCensus(HWSceneSurfaceProfile* profile, seg_t* seg, sector_t* front, sector_t* back)
			: Profile(profile), Seg(seg), Front(front), Back(back)
		{
			BeforeSubmitted = profile ? profile->Submitted : 0;
			BeforeOrdinaryOpaque = profile ? profile->OpaqueRegular : 0;
			BeforeTop = profile ? profile->OutOrdinaryTop : 0;
			BeforeBottom = profile ? profile->OutOrdinaryBottom : 0;
			BeforeM2S = profile ? profile->OutM2S : 0;
			if (profile) Timer.ResetAndClock();
		}
		~FProcessOutputCensus()
		{
			if (!Profile) return;
			Timer.Unclock();
			const uint64_t ns = uint64_t(Timer.Time() * 1e9);
			Profile->CensusProcessNS += ns;
			const uint64_t submitted = Profile->Submitted - BeforeSubmitted;
			const uint64_t ordinary = Profile->OpaqueRegular - BeforeOrdinaryOpaque;
			Profile->ProcessWallSubmissions += submitted;
			Profile->ProcessOrdinaryOpaqueParts += ordinary;
			if (submitted) ++Profile->ProcessCallsWithWallSubmission;
			else
			{
				++Profile->ProcessCallsWithoutWallSubmission;
				Profile->CensusEmptyProcessNS += ns;
				bool cert = false;
				if (Back && Front && Seg && Seg->sidedef && Seg->linedef)
				{
					auto midtex = TexMan.GetGameTexture(Seg->sidedef->GetTexture(side_t::mid), true);
					const bool noMid = (!midtex || !midtex->isValid());
					const bool noPortal = !Seg->linedef->isVisualPortal() &&
						Seg->linedef->special != Line_Horizon && Seg->linedef->special != Line_Mirror &&
						!Seg->linedef->GetTransferredPortal();
					const bool equalPlanes = !Front->floorplane.isSlope() && !Back->floorplane.isSlope() &&
						!Front->ceilingplane.isSlope() && !Back->ceilingplane.isSlope() &&
						Front->floorplane.D == Back->floorplane.D && Front->ceilingplane.D == Back->ceilingplane.D;
					cert = noMid && noPortal && equalPlanes;
				}
				if (cert)
				{
					++Profile->CensusEmptyEqualCandidate;
					Profile->CensusEmptyEqualCandidateNS += ns;
				}
			}
			if (ordinary) ++Profile->ProcessCallsWithOrdinaryOpaque;
			const uint64_t top = Profile->OutOrdinaryTop - BeforeTop;
			const uint64_t bottom = Profile->OutOrdinaryBottom - BeforeBottom;
			const uint64_t mid = Profile->OutM2S - BeforeM2S;
			if (mid)
			{
				++Profile->CensusMidNativeSides;
				Profile->CensusMidNativeTop += top;
				Profile->CensusMidNativeBottom += bottom;
				Profile->CensusMidNativeProcessNS += ns;
				if (!top && !bottom) ++Profile->CensusMidOnlySides;
			}
		}
	};
	HWSceneSurfaceProfile* censusProfile = (SceneProfiling && !di->SuppressSubmit) ? &GetSceneSurfaceProfile() : nullptr;
	FProcessOutputCensus outputCensus(censusProfile, seg, front, back);
	Prepared = nullptr;
	GpuOwnerCandidate = nullptr;
	GpuResidentEpoch = 0;
	GpuResidentRow = UINT32_MAX;
	GpuOwnerSlot = -1;
	GpuAppearanceId = UINT32_MAX;
	GpuAppearanceRevision = 0;
	if (di->mh)
	{
		PrepareWall(di, seg, front, back, culled);
		return;
	}
	if (TryGpuPersistentWall(*this, di, seg, front, back, culled)) return;
	struct FGpuPersistentCapture
	{
		FGpuPersistentWall* Owner;
		~FGpuPersistentCapture()
		{
			if (Owner) Owner->Valid = !Owner->CaptureRejected;
		}
	} gpuCapture { GpuOwnerCandidate };
	RetainedWorld::SideInputs inputs;
	RetainedWorld::Entry* found = nullptr;
	bool hit = false;
	cycle_t validation;
	if (SceneProfiling) validation.ResetAndClock();
	const bool eligible = vk_retainedworld && RetainedWorld::Snapshot(di, seg, front, back, culled, inputs, found, hit);
	if (SceneProfiling)
	{
		validation.Unclock();
		GetSceneSurfaceProfile().PreparedValidationNS += uint64_t(validation.Time() * 1e9);
	}
	const bool owns = di->di && di->di->OwnOpaqueWalls && di->SubmissionOrder && !di->SuppressSubmit;
	bool selectOwned = false;
	if (owns)
	{
		HWSceneScopeTimer admissionTimer(GetSceneSurfaceProfile().OwnedAdmissionNS);
		EOwnedAdmissionReason reason = OAR_Admitted;
		if (culled) reason = OAR_Culled;
		else if (front != seg->frontsector || back != seg->backsector || seg->Subsector->sector != front) reason = OAR_FakeSectorMismatch;
		else if (seg->sidedef->linedef != seg->linedef) reason = OAR_SideLineMismatch;
		else if (seg->sidedef->Flags & (WALLF_POLYOBJ | WALLF_BLOCKRENDERING | WALLF_DITHERTRANS_TOP | WALLF_DITHERTRANS_BOTTOM | WALLF_DITHERTRANS_MID)) reason = OAR_BlockFlags;
		else if (!eligible) reason = OAR_RetainedRecipeUnavailable;
		else if (inputs.EmptyOnly)
		{
			const auto& empty = RetainedWorld::EmptySides;
			const RetainedWorld::EmptyInputs live = { front, back, inputs.FrontVersion, inputs.BackVersion, inputs.LineFlags };
			if (empty.empty() || !empty[seg->sidedef->Index()].Valid || !(empty[seg->sidedef->Index()].Inputs == live)) reason = OAR_ColdRecipe;
		}
		else if (!hit) reason = OAR_ColdRecipe;
		selectOwned = reason == OAR_Admitted;
		if (SceneProfiling)
		{
			auto& profile = GetSceneSurfaceProfile();
			++profile.OwnedAdmissionCalls;
			++profile.OwnedAdmissionReasons[reason];
			if (selectOwned) ++profile.OwnedCandidates;
		}
	}
	bool ownedRejected = false;
	if (selectOwned)
	{
		HWSceneScopeTimer ownedTimer(GetSceneSurfaceProfile().OwnedPrepareNS);
		if (inputs.EmptyOnly)
		{
			if (SceneProfiling)
			{
				++GetSceneSurfaceProfile().OwnedEmptySelections;
				++GetSceneSurfaceProfile().PreparedHits;
			}
			return;
		}
		if (SelectOwnedWall(di, *this, seg, front, back, *found, inputs))
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedHits;
			return;
		}
		ownedRejected = true;
		if (SceneProfiling)
		{
			++GetSceneSurfaceProfile().OwnedRejected;
			++GetSceneSurfaceProfile().OwnedInlineReplayFallbacks;
		}
	}
	if (!eligible)
	{
		if (SceneProfiling && vk_retainedworld) ++GetSceneSurfaceProfile().PreparedFallbacks;
		PrepareWall(di, seg, front, back, culled);
		return;
	}
	if (inputs.EmptyOnly)
	{
		RetainedWorld::ProcessEmpty(*this, di, seg, front, back, inputs);
		return;
	}
	if (RetainedWorld::SideSlots.empty()) RetainedWorld::SideSlots.resize(di->Level->sides.Size());
	if (!hit && RetainedWorld::Geometry.size() + 3 > RetainedWorld::Capacity)
	{
		RetainedWorld::Fallback(6);
		if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedFallbacks;
		PrepareWall(di, seg, front, back, culled);
		return;
	}
	auto replay = [&]()
	{
		cycle_t timer;
		if (SceneProfiling) timer.ResetAndClock();
		RetainedWorld::ReplayViewEffects(di, *this, seg, front, back);
		RetainedWorld::ReplayEntry(di, *this, *found, &inputs);
		if (SceneProfiling)
		{
			timer.Unclock();
			GetSceneSurfaceProfile().PreparedReplayNS += uint64_t(timer.Time() * 1e9);
			if (ownedRejected) GetSceneSurfaceProfile().OwnedFallbackNS += uint64_t(timer.Time() * 1e9);
		}
	};
	if (hit && !vk_retainedworld_verify)
	{
		di->PreparedReplay = &found->Output;
		if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedHits;
		replay();
		di->PreparedReplay = nullptr;
		return;
	}
	HWMeshHelper capture;
	if (hit)
	{
		di->PreparedReplay = &found->Output;
		if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedHits;
		if (vk_retainedworld_verify) di->PreparedCapture = &capture;
	}
	else
	{
		di->PreparedCapture = &capture;
		if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedRebuilds;
	}
	if (hit) replay();
	else PrepareWall(di, seg, front, back, culled);
	di->PreparedReplay = nullptr;
	di->PreparedCapture = nullptr;
	if (hit && vk_retainedworld_verify)
	{
		HWMeshHelper reference;
		di->PreparedCapture = &reference;
		di->SuppressSubmit = true;
		PrepareWall(di, seg, front, back, culled);
		di->PreparedCapture = nullptr;
		di->SuppressSubmit = false;
		auto compareMissing = [](const TArray<HWMissing>& cached, const TArray<HWMissing>& original)
		{
			if (cached.Size() != original.Size()) return false;
			for (unsigned i = 0; i < cached.Size(); ++i)
				if (cached[i].side != original[i].side || cached[i].plane != original[i].plane) return false;
			return true;
		};
		if (!compareMissing(found->Output.upper, reference.upper) || !compareMissing(found->Output.lower, reference.lower))
			I_FatalError("Retained missing-texture recipe differs on side %d", seg->sidedef->Index());
		if (capture.list.Size() != reference.list.Size()) I_FatalError("Retained wall output count differs on side %d", seg->sidedef->Index());
		for (unsigned i = 0; i < capture.list.Size(); ++i)
		{
			FFlatVertex quad[4];
			auto ptr = quad;
			reference.list[i].CreateVertices(ptr, false);
			auto& cached = capture.list[i];
			auto& original = reference.list[i];
			if (!cached.Prepared || memcmp(quad, cached.Prepared->Vertices.data(), sizeof(quad)) ||
				cached.type != original.type || cached.flags != original.flags || cached.texture != original.texture ||
				cached.alpha != original.alpha || cached.RenderStyle != original.RenderStyle ||
				memcmp(&cached.glseg, &original.glseg, sizeof(cached.glseg)) ||
				cached.lightlevel != original.lightlevel || cached.rellight != original.rellight ||
				memcmp(&cached.Colormap, &original.Colormap, sizeof(FColormap)) ||
				((cached.flags & HWF_GLOW) && (memcmp(cached.topglowcolor, original.topglowcolor, sizeof(cached.topglowcolor)) ||
				memcmp(cached.bottomglowcolor, original.bottomglowcolor, sizeof(cached.bottomglowcolor)))))
			{
				I_FatalError("Retained wall reference differs on side %d part %d", seg->sidedef->Index(), original.type);
			}
		}
		if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedVerified;
	}
	else if (!hit)
	{
		RetainedWorld::StoreSideEntry(seg->sidedef->Index(), inputs, std::move(capture));
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWWall::PrepareWall(HWWallDispatcher *di, seg_t *seg, sector_t * frontsector, sector_t * backsector, bool isculled)
{
	struct PreparationTimer
	{
		bool Active;
		cycle_t Timer;
		PreparationTimer(bool active) : Active(active)
		{
			if (Active)
			{
				Timer.ResetAndClock();
			}
		}
		~PreparationTimer()
		{
			if (Active)
			{
				Timer.Unclock();
				GetSceneSurfaceProfile().PreparedBuildNS += uint64_t(Timer.Time() * 1e9);
			}
		}
	} preparationTimer(SceneProfiling && !di->SuppressSubmit);
	vertex_t * v1, *v2;
	float fch1;
	float ffh1;
	float fch2;
	float ffh2;
	float frefz, crefz;
	sector_t * realfront;
	sector_t * realback;
	sector_t * segfront;
	sector_t * segback;

#ifdef _DEBUG
	if (seg->linedef->Index() == 759)
	{
		int a = 0;
	}
#endif

	lightmap = nullptr;

	// note: we always have a valid sidedef and linedef reference when getting here.

	this->seg = seg;
	this->frontsector = frontsector;
	this->backsector = backsector;
	vertindex = 0;
	vertcount = 0;

	if ((seg->sidedef->Flags & WALLF_POLYOBJ) && seg->backsector)
	{
		// Textures on 2-sided polyobjects are aligned to the actual seg's sectors
		segfront = realfront = seg->frontsector;
		segback = realback = seg->backsector;
	}
	else
	{
		// Need these for aligning the textures
		realfront = &di->Level->sectors[frontsector->sectornum];
		realback = backsector ? &di->Level->sectors[backsector->sectornum] : NULL;
		segfront = frontsector;
		segback = backsector;
	}
	frefz = realfront->GetPlaneTexZ(sector_t::floor);
	crefz = realfront->GetPlaneTexZ(sector_t::ceiling);

	// [Karto] Shader plane heights: only tiers drawn straight from the real sectors' planes name them; fake
	// (Transfer_Heights) copies, polyobjects and portal-aligned backs keep producer-only geometry.
	HeightArmed = HeightValid = false;
	const bool heightPlanes = segfront == realfront && segback == realback && !(seg->sidedef->Flags & WALLF_POLYOBJ) &&
		!seg->linedef->isVisualPortal();
	auto armHeights = [&](sector_t* top, int topPlane, sector_t* bottom, int bottomPlane, sector_t* anchor, int anchorPlane)
	{
		HeightArmed = heightPlanes;
		HeightTop = top->Index() * 2 + topPlane;
		HeightBottom = bottom->Index() * 2 + bottomPlane;
		HeightAnchor = anchor->Index() * 2 + anchorPlane;
	};

	if (seg->sidedef == seg->linedef->sidedef[0])
	{
		v1 = seg->linedef->v1;
		v2 = seg->linedef->v2;
	}
	else
	{
		v1 = seg->linedef->v2;
		v2 = seg->linedef->v1;
	}

	if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		glseg.fracleft = 0;
		glseg.fracright = 1;
		if (gl_seamless)
		{
			if (v1->dirty) v1->RecalcVertexHeights();
			if (v2->dirty) v2->RecalcVertexHeights();
		}
	}
	else	// polyobjects must be rendered per seg.
	{
		if (fabs(v1->fX() - v2->fX()) > fabs(v1->fY() - v2->fY()))
		{
			glseg.fracleft = (seg->v1->fX() - v1->fX()) / (v2->fX() - v1->fX());
			glseg.fracright = (seg->v2->fX() - v1->fX()) / float(v2->fX() - v1->fX());
		}
		else
		{
			glseg.fracleft = (seg->v1->fY() - v1->fY()) / (v2->fY() - v1->fY());
			glseg.fracright = (seg->v2->fY() - v1->fY()) / (v2->fY() - v1->fY());
		}
		v1 = seg->v1;
		v2 = seg->v2;
		flags |= HWF_NOSPLITUPPER | HWF_NOSPLITLOWER;	// seg-splitting not needed for single segs.
	}


	vertexes[0] = v1;
	vertexes[1] = v2;

	glseg.x1 = v1->fX();
	glseg.y1 = v1->fY();
	glseg.x2 = v2->fX();
	glseg.y2 = v2->fY();
	Colormap = frontsector->Colormap;
	flags = 0;
	dynlightindex = -1;
	lightlist = NULL;

	int rel = 0;
	int orglightlevel = hw_ClampLight(frontsector->lightlevel);
	bool foggy = (!Colormap.FadeColor.isBlack() || di->Level->flags&LEVEL_HASFADETABLE);	// fog disables fake contrast

	alpha = 1.0f;
	RenderStyle = STYLE_Normal;
	texture = NULL;


	if (frontsector->GetWallGlow(topglowcolor, bottomglowcolor)) flags |= HWF_GLOW;

	zfloor[0] = ffh1 = segfront->floorplane.ZatPoint(v1);
	zfloor[1] = ffh2 = segfront->floorplane.ZatPoint(v2);
	zceil[0] = fch1 = segfront->ceilingplane.ZatPoint(v1);
	zceil[1] = fch2 = segfront->ceilingplane.ZatPoint(v2);

	if (seg->linedef->special == Line_Horizon)
	{
		SkyNormal(di, frontsector, v1, v2);
		DoHorizon(di, seg, frontsector, v1, v2);
		return;
	}

	if (isculled)
	{
		// [Karto] The native cutoff emits a top wall before ordinary tier setup; initialize lighting so producer bypass cannot expose stale stack fields.
		lightlevel = RescaleLightLevel(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::top, false, &rel));
		rellight = CalcRelLight(lightlevel, orglightlevel, rel);
		if (frontsector->GetTexture(sector_t::ceiling) == skyflatnum)
		{
			SkyNormal(di, frontsector, v1, v2);
		}
		else
		{
			texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::ceiling), true);
			if (texture && texture->isValid())
			{
				DoTexture(di, RENDERWALL_TOP, seg, true,
					crefz, frefz,
					fch1, fch2, ffh1, ffh2, 0, 0);
			}
		}
		return;
	}

	bool isportal = seg->linedef->isVisualPortal() && seg->sidedef == seg->linedef->sidedef[0];

	//return;
	// [GZ] 3D middle textures are necessarily two-sided, even if they lack the explicit two-sided flag
	if (!backsector || (!(seg->linedef->flags&(ML_TWOSIDED | ML_3DMIDTEX)) && !isportal)) // one sided
	{
		// sector's sky
		SkyNormal(di, frontsector, v1, v2);

		if (di->PreparedReplay)
		{
			RetainedWorld::Replay(di, *this);
			return;
		}
		if (isportal)
		{
			lineportal = seg->linedef->getPortal()->mGroup;
			ztop[0] = zceil[0];
			ztop[1] = zceil[1];
			zbottom[0] = zfloor[0];
			zbottom[1] = zfloor[1];
			PutPortal(di, PORTALTYPE_LINETOLINE, -1);
		}
		else if (seg->linedef->GetTransferredPortal())
		{
			SkyLine(di, frontsector, seg->linedef);
		}
		else
		{
			// normal texture
			lightlevel = RescaleLightLevel(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::mid, false, &rel));
			rellight = CalcRelLight(lightlevel, orglightlevel, rel);
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::mid), true);
			if (texture && texture->isValid())
			{
				int skewflag = seg->sidedef->textures[side_t::mid].skew;
				if (skewflag == 0) skewflag = midskew;
				float skew =
					skewflag == side_t::skew_front_ceiling ? fch2 - fch1 :
					skewflag == side_t::skew_front_floor ? ffh2 - ffh1 : 0.;
				armHeights(realfront, sector_t::ceiling, realfront, sector_t::floor, realfront,
					(seg->linedef->flags & ML_DONTPEGBOTTOM) ? sector_t::floor : sector_t::ceiling);
				DoTexture(di, RENDERWALL_M1S, seg, (seg->linedef->flags & ML_DONTPEGBOTTOM) > 0,
					crefz, frefz,	// must come from the original!
					fch1, fch2, ffh1, ffh2, 0, skew);
			}
		}
	}
	else // two sided
	{
		float bfh1 = segback->floorplane.ZatPoint(v1);
		float bfh2 = segback->floorplane.ZatPoint(v2);
		float bch1 = segback->ceilingplane.ZatPoint(v1);
		float bch2 = segback->ceilingplane.ZatPoint(v2);
		float zalign = 0.f;


		if (isportal && seg->backsector == nullptr)
		{
			SkyNormal(di, frontsector, v1, v2); // For sky rendering purposes this needs to be treated as a one-sided wall.

			// If this is a one-sided portal and we got floor or ceiling alignment, the upper/lower texture position needs to be adjusted for that.
			// (We assume that this portal won't involve slopes!)
			switch (seg->linedef->getPortalAlignment())
			{
			case PORG_FLOOR:
				zalign = ffh1 - bfh1;
				bch1 += zalign;
				bch2 += zalign;
				bfh1 += zalign;
				bfh2 += zalign;
				return;

			case PORG_CEILING:
				zalign = fch1 - bch1;
				bch1 += zalign;
				bch2 += zalign;
				bfh1 += zalign;
				bfh2 += zalign;
				return;

			default:
				break;
			}
		}
		else
		{
			SkyTop(di, seg, frontsector, backsector, v1, v2);
			SkyBottom(di, seg, frontsector, backsector, v1, v2);
		}
		if (di->PreparedReplay)
		{
			RetainedWorld::Replay(di, *this);
			return;
		}

		// upper texture
		if (frontsector->GetTexture(sector_t::ceiling) != skyflatnum || backsector->GetTexture(sector_t::ceiling) != skyflatnum)
		{
			float bch1a = bch1, bch2a = bch2;
			if (frontsector->GetTexture(sector_t::floor) != skyflatnum || backsector->GetTexture(sector_t::floor) != skyflatnum)
			{
				// the back sector's floor obstructs part of this wall
				if (ffh1 > bch1 && ffh2 > bch2 && (seg->linedef->flags & ML_DRAWFULLHEIGHT) == 0)
				{
					bch2a = ffh2;
					bch1a = ffh1;
				}
			}

			if (bch1a < fch1 || bch2a < fch2)
			{
				lightlevel = RescaleLightLevel(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::top, false, &rel));
				rellight = CalcRelLight(lightlevel, orglightlevel, rel);
				texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::top), true);
				if (texture && texture->isValid())
				{
					int skewflag = seg->sidedef->textures[side_t::top].skew;
					float skew;
					if (skewflag == 0) skewflag = topskew;

					switch (skewflag)
					{
					default:
						skew = 0;
						break;
					case side_t::skew_front_ceiling:
						skew = fch2 - fch1;
						break;
					case side_t::skew_back_ceiling:
						skew = bch2 - bch1;
						break;
					case side_t::skew_front_floor:
						skew = ffh2 - ffh1;
						break;
					case side_t::skew_back_floor:
						skew = bfh2 - bfh1;
						break;
					}
					armHeights(realfront, sector_t::ceiling, bch1a != bch1 || bch2a != bch2 ? realfront : realback,
						bch1a != bch1 || bch2a != bch2 ? sector_t::floor : sector_t::ceiling,
						(seg->linedef->flags & ML_DONTPEGTOP) ? realfront : realback, sector_t::ceiling);
					DoTexture(di, RENDERWALL_TOP, seg, (seg->linedef->flags & (ML_DONTPEGTOP)) == 0,
						crefz, realback->GetPlaneTexZ(sector_t::ceiling),
						fch1, fch2, bch1a, bch2a, 0, skew);
				}
				else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
				{
					if ((frontsector->ceilingplane.isSlope() || backsector->ceilingplane.isSlope()) &&
						frontsector->GetTexture(sector_t::ceiling) != skyflatnum &&
						backsector->GetTexture(sector_t::ceiling) != skyflatnum)
					{
						texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::ceiling), true);
						if (texture && texture->isValid())
						{
							armHeights(realfront, sector_t::ceiling, bch1a != bch1 || bch2a != bch2 ? realfront : realback,
								bch1a != bch1 || bch2a != bch2 ? sector_t::floor : sector_t::ceiling,
								(seg->linedef->flags & ML_DONTPEGTOP) ? realfront : realback, sector_t::ceiling);
							DoTexture(di, RENDERWALL_TOP, seg, (seg->linedef->flags & (ML_DONTPEGTOP)) == 0,
								crefz, realback->GetPlaneTexZ(sector_t::ceiling),
								fch1, fch2, bch1a, bch2a, 0, 0);
						}
					}
					else
					{
						// skip processing if the back is a malformed subsector
							if (di->PreparedCapture && !di->PreparedReplay) di->PreparedCapture->upper.Push({ seg->sidedef, nullptr, bch1a });
							if (seg->PartnerSeg != NULL && !(seg->PartnerSeg->Subsector->hacked & 4))
						{
							RejectGpuPersistentWall(this);
						di->AddUpperMissingTexture(seg->sidedef, sub, bch1a);
						}
					}
				}
			}
		}


		/* mid texture */
		sector_t *backsec = isportal? seg->linedef->getPortalDestination()->frontsector : backsector;

		bool drawfogboundary = !di->isFullbrightScene() && CheckFog(di->Level, frontsector, backsec, di->lightmode);
		auto tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::mid), true);
		if (tex != NULL && tex->isValid())
		{
			if (di->Level->i_compatflags & COMPATF_MASKEDMIDTEX)
			{
				auto rawtexid = TexMan.GetRawTexture(tex->GetID());
				auto rawtex = TexMan.GetGameTexture(rawtexid);
				if (rawtex) tex = rawtex;
			}
			texture = tex;
		}
		else texture = nullptr;
		lightlevel = RescaleLightLevel(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::mid, false, &rel));
		rellight = CalcRelLight(lightlevel, orglightlevel, rel);

		float skew;
		int skewflag = seg->sidedef->textures[side_t::mid].skew;
		if (skewflag == 0) skewflag = midskew;
		switch (skewflag)
		{
		default:
			skew = 0;
			break;
		case side_t::skew_front_ceiling:
			skew = fch2 - fch1;
			break;
		case side_t::skew_back_ceiling:
			skew = bch2 - bch1;
			break;
		case side_t::skew_front_floor:
			skew = ffh2 - ffh1;
			break;
		case side_t::skew_back_floor:
			skew = bfh2 - bfh1;
			break;
		}

		if (isportal)
		{
			lineportal = seg->linedef->getPortal()->mGroup;
			ztop[0] = bch1;
			ztop[1] = bch2;
			zbottom[0] = bfh1;
			zbottom[1] = bfh2;
			PutPortal(di, PORTALTYPE_LINETOLINE, -1);

			if (texture && seg->backsector != nullptr)
			{
				DoMidTexture(di, seg, drawfogboundary, frontsector, backsector, realfront, realback,
					fch1, fch2, ffh1, ffh2, bch1, bch2, bfh1, bfh2, zalign, skew);
			}
		}
		else
		{

			if (texture || drawfogboundary)
			{
				DoMidTexture(di, seg, drawfogboundary, frontsector, backsector, realfront, realback,
					fch1, fch2, ffh1, ffh2, bch1, bch2, bfh1, bfh2, zalign, skew);
			}

			if (backsector->e->XFloor.ffloors.Size() || frontsector->e->XFloor.ffloors.Size())
			{
				lightlevel = RescaleLightLevel(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::top, false, &rel));
				rellight = CalcRelLight(lightlevel, orglightlevel, rel);
				DoFFloorBlocks(di, seg, frontsector, backsector, fch1, fch2, ffh1, ffh2, bch1, bch2, bfh1, bfh2);
			}
		}

		/* bottom texture */
		// the back sector's ceiling obstructs part of this wall (specially important for sky sectors)
		float bfh1a = bfh1, bfh2a = bfh2;
		bool lowerObstructed = false;
		if (fch1 < bfh1 && fch2 < bfh2 && (seg->linedef->flags & ML_DRAWFULLHEIGHT) == 0)
		{
			bfh1 = fch1;
			bfh2 = fch2;
			lowerObstructed = true;
		}

		if (bfh1 > ffh1 || bfh2 > ffh2)
		{
			lightlevel = RescaleLightLevel(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::bottom, false, &rel));
			rellight = CalcRelLight(lightlevel, orglightlevel, rel);
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::bottom), true);
			if (texture && texture->isValid())
			{
				int skewflag = seg->sidedef->textures[side_t::bottom].skew;
				if (skewflag == 0) skewflag = bottomskew;
				float skew;
				switch (skewflag)
				{
				default:
					skew = 0;
					break;
				case side_t::skew_front_ceiling:
					skew = fch2 - fch1;
					break;
				case side_t::skew_back_ceiling:
					skew = bch2 - bch1;
					break;
				case side_t::skew_front_floor:
					skew = ffh2 - ffh1;
					break;
				case side_t::skew_back_floor:
					skew = bfh2a - bfh1a;
					break;
				}

				armHeights(lowerObstructed ? realfront : realback, lowerObstructed ? sector_t::ceiling : sector_t::floor,
					realfront, sector_t::floor,
					!(seg->linedef->flags & ML_DONTPEGBOTTOM) ? realback :
					frontsector->GetTexture(sector_t::ceiling) == skyflatnum && backsector->GetTexture(sector_t::ceiling) == skyflatnum ? realback : realfront,
					!(seg->linedef->flags & ML_DONTPEGBOTTOM) ? sector_t::floor : sector_t::ceiling);
				DoTexture(di, RENDERWALL_BOTTOM, seg, (seg->linedef->flags & ML_DONTPEGBOTTOM) > 0,
					realback->GetPlaneTexZ(sector_t::floor), frefz,
					bfh1, bfh2, ffh1, ffh2,
					frontsector->GetTexture(sector_t::ceiling) == skyflatnum && backsector->GetTexture(sector_t::ceiling) == skyflatnum ?
					frefz - realback->GetPlaneTexZ(sector_t::ceiling) :
					frefz - crefz, skew);
			}
			else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
			{
				if ((frontsector->floorplane.isSlope() || backsector->floorplane.isSlope()) &&
					frontsector->GetTexture(sector_t::floor) != skyflatnum &&
					backsector->GetTexture(sector_t::floor) != skyflatnum)
				{
					// render it anyway with the sector's floor texture. With a background sky
					// there are ugly holes otherwise and slopes are simply not precise enough
					// to mach in any case.
					texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::floor), true);
					if (texture && texture->isValid())
					{
						armHeights(lowerObstructed ? realfront : realback, lowerObstructed ? sector_t::ceiling : sector_t::floor,
							realfront, sector_t::floor, (seg->linedef->flags & ML_DONTPEGBOTTOM) ? realfront : realback,
							(seg->linedef->flags & ML_DONTPEGBOTTOM) ? sector_t::ceiling : sector_t::floor);
						DoTexture(di, RENDERWALL_BOTTOM, seg, (seg->linedef->flags & ML_DONTPEGBOTTOM) > 0,
							realback->GetPlaneTexZ(sector_t::floor), frefz,
							bfh1, bfh2, ffh1, ffh2, frefz - crefz, 0);
					}
				}
				else if (backsector->GetTexture(sector_t::floor) != skyflatnum)
				{
					// skip processing if the back is a malformed subsector
						if (di->PreparedCapture && !di->PreparedReplay) di->PreparedCapture->lower.Push({ seg->sidedef, nullptr, bfh1 });
						if (seg->PartnerSeg != NULL && !(seg->PartnerSeg->Subsector->hacked & 4))
					{
						RejectGpuPersistentWall(this);
						di->AddLowerMissingTexture(seg->sidedef, sub, bfh1);
					}
				}
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWWall::ProcessLowerMiniseg(HWWallDispatcher *di, seg_t *seg, sector_t * frontsector, sector_t * backsector)
{
	if (frontsector->GetTexture(sector_t::floor) == skyflatnum) return;
	lightlist = NULL;

	float ffh = frontsector->GetPlaneTexZ(sector_t::floor);
	float bfh = backsector->GetPlaneTexZ(sector_t::floor);

	vertindex = 0;
	vertcount = 0;
	if (bfh > ffh)
	{
		this->seg = seg;
		this->frontsector = frontsector;
		this->backsector = backsector;
		this->sub = NULL;
		this->lightmap = nullptr;

		vertex_t * v1 = seg->v1;
		vertex_t * v2 = seg->v2;
		vertexes[0] = v1;
		vertexes[1] = v2;

		glseg.x1 = v1->fX();
		glseg.y1 = v1->fY();
		glseg.x2 = v2->fX();
		glseg.y2 = v2->fY();
		glseg.fracleft = 0;
		glseg.fracright = 1;

		flags = 0;

		// can't do fake contrast without a sidedef
		lightlevel = RescaleLightLevel(frontsector->lightlevel);
		rellight = 0;

		alpha = 1.0f;
		RenderStyle = STYLE_Normal;
		Colormap = frontsector->Colormap;

		if (frontsector->GetWallGlow(topglowcolor, bottomglowcolor)) flags |= HWF_GLOW;
		dynlightindex = -1;

		zfloor[0] = zfloor[1] = ffh;

		texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::floor), true);

		if (texture && texture->isValid())
		{
			FTexCoordInfo tci;
			type = RENDERWALL_BOTTOM;
			tci.GetFromTexture(texture, 1, 1, false);
			SetWallCoordinates(seg, &tci, bfh, bfh, bfh, ffh, ffh, 0, 0);
			PutWall(di, false);
		}
	}
}
