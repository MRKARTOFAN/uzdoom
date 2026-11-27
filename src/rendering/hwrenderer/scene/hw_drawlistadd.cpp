/*
** hw_drawlistadd.cpp
**
** A short, one line description of what this file does
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

#include "hw_dynlightdata.h"
#include "hw_cvars.h"
#include "hw_lightbuffer.h"

CVAR(Bool, r_sprite_opaquepass, true, 0)
CVAR(Bool, r_sprite_addpass, true, 0)
CVAR(Bool, r_sprite_transbuckets, true, 0)
CVAR(Bool, r_particle_solidpass, true, 0)
CVAR(Bool, r_particle_addpass, true, 0)
EXTERN_CVAR(Bool, gl_light_particles)
#include "v_video.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hw_material.h"
#include "actor.h"
#include "g_levellocals.h"
#include "c_dispatch.h"
#include "hw_spriterecord.h"

EXTERN_CVAR(Bool, gl_seamless)

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddWall(HWWall *wall, bool retained, unsigned order, unsigned part, bool ownedBatch)
{
	if (wall->flags & HWWall::HWF_TRANSLUCENT)
	{
		auto newwall = drawlists[GLDL_TRANSLUCENT].NewWall();
		*newwall = *wall;
	}
	else
	{
		bool masked = HWWall::passflag[wall->type] == 1 ? false : (wall->texture && wall->texture->isMasked());
		int list;

		if (wall->flags & HWWall::HWF_SKYHACK && wall->type == RENDERWALL_M2S)
		{
			list = GLDL_MASKEDWALLSOFS;
		}
		else
		{
			list = masked ? GLDL_MASKEDWALLS : GLDL_PLAINWALLS;
		}
		auto newwall = drawlists[list].NewWall(retained ? wall : nullptr);
		if (!retained) *newwall = *wall;
		if (OwnOpaqueWalls && list == GLDL_PLAINWALLS)
		{
			auto& submissions = ownedBatch ? drawlists[list].OwnedWalls : drawlists[list].FallbackWalls;
			submissions.Push({ newwall, 0, order, order ? part : submissions.Size() });
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddMirrorSurface(HWWall *w)
{
	w->type = RENDERWALL_MIRRORSURFACE;
	auto newwall = drawlists[GLDL_TRANSLUCENTBORDER].NewWall();
	*newwall = *w;

	// Invalidate vertices to allow setting of texture coordinates
	newwall->vertcount = 0;

	FVector3 v = newwall->glseg.Normal();
	auto tcs = newwall->tcs;
	tcs[HWWall::LOLFT].u = tcs[HWWall::LORGT].u = tcs[HWWall::UPLFT].u = tcs[HWWall::UPRGT].u = v.X;
	tcs[HWWall::LOLFT].v = tcs[HWWall::LORGT].v = tcs[HWWall::UPLFT].v = tcs[HWWall::UPRGT].v = v.Z;
	newwall->MakeVertices(false);

	bool hasDecals = newwall->seg->sidedef && newwall->seg->sidedef->AttachedDecals;
	if (hasDecals && Level->HasDynamicLights && !isFullbrightScene())
	{
		newwall->SetupLights(this, lightdata);
	}
	newwall->ProcessDecals(this);
	newwall->dynlightindex = -1; // the environment map should not be affected by lights - only the decals.
}

//==========================================================================
//
// FDrawInfo::AddFlat
//
// Checks texture, lighting and translucency settings and puts this
// plane in the appropriate render list.
//
//==========================================================================

void HWDrawInfo::AddFlat(HWFlat *flat, bool fog)
{
	int list;

	if (flat->renderstyle != STYLE_Translucent || flat->alpha < 1.f - FLT_EPSILON || fog || flat->texture == nullptr)
	{
		// translucent 3D floors go into the regular translucent list, translucent portals go into the translucent border list.
		list = (flat->renderflags&SSRF_RENDER3DPLANES) ? GLDL_TRANSLUCENT : GLDL_TRANSLUCENTBORDER;
	}
	else if (flat->texture->GetTranslucency())
	{
		if (flat->stack)
		{
			list = GLDL_TRANSLUCENTBORDER;
		}
		else if ((flat->renderflags&SSRF_RENDER3DPLANES) && !flat->plane.plane.isSlope())
		{
			list = GLDL_TRANSLUCENT;
		}
		else
		{
			list = GLDL_PLAINFLATS;
		}
	}
	else //if (flat->hacktype != SSRF_FLOODHACK) // The flood hack may later need different treatment but with the current setup can go into the existing render list.
	{
		bool masked = flat->texture->isMasked() && ((flat->renderflags&SSRF_RENDER3DPLANES) || flat->stack);
		list = masked ? GLDL_MASKEDFLATS : GLDL_PLAINFLATS;
	}
	auto newflat = drawlists[list].NewFlat();
	*newflat = *flat;
}


//==========================================================================
//
//
//
//==========================================================================
static FILE* SpriteDumpFile;

CCMD(sprite_dump)
{
	if (argv.argc() != 2) { Printf("Usage: sprite_dump <file>\n"); return; }
	if (SpriteDumpFile) fclose(SpriteDumpFile);
	SpriteDumpFile = fopen(argv[1], "wt");
	Printf("SPRITEDUMP %s %s\n", argv[1], SpriteDumpFile ? "armed" : "failed");
}

void SpriteDumpEndFrame()
{
	if (!SpriteDumpFile) return;
	fclose(SpriteDumpFile);
	SpriteDumpFile = nullptr;
	Printf("SPRITEDUMP written\n");
}

static void SpriteDumpWrite(HWDrawInfo* di, HWSprite* s, int list)
{
	const void* id = s->record ? (const void*)s->record->Live : s->actor ? (const void*)WorldModeLiveActor(s->actor) : (const void*)s->particle;
	const bool copy = s->record == nullptr && s->actor != nullptr && (const void*)s->actor != id;
	fprintf(SpriteDumpFile, "%d %p %p %d %s %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %08x %d %.3f %d\n", list, id, (void*)di, int(copy),
		s->texture ? s->texture->GetName().GetChars() : "-", s->lightlevel, s->x1, s->y1, s->z1, s->x2, s->y2, s->z2, s->x, s->y, s->z,
		s->RenderStyle.AsDWORD, s->translation.index(), s->trans, int(s->fullbright));
}

CVAR(Bool, r_particle_lit, true, 0)

void HWDrawInfo::AddSprite(HWSprite *sprite, bool translucent)
{
	int list;
	// [Karto] Additive blending with depth writes off is order-free, so these need no depth sort; world mode only, since
	// its acceptance is visual equivalence.
	if (r_sprite_addpass && translucent && sprite->modelframe == nullptr && sprite->RenderStyle.BlendOp == STYLEOP_Add &&
		sprite->RenderStyle.DestAlpha == STYLEALPHA_One && sprite->lightlist == nullptr && sprite->topclip == LARGE_VALUE &&
		sprite->bottomclip == -LARGE_VALUE && (sprite->record != nullptr ||
		(sprite->actor != nullptr && (sprite->actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)) &&
		screen->BuffersArePersistent() && WorldModeActive(Level))
	{
		list = GLDL_ADDSPRITES;
	}
	// [Karto] Drawn after the sorted list by draw state, so alpha items overlapping each other and sprites against translucent
	// walls lose depth order. Only what the bucket draw reproduces exactly: no light slices, clipping, model, fog layer, fuzz
	// or subtractive style; particles also without desaturation or own dynamic light.
	else if (r_sprite_transbuckets && translucent && sprite->modelframe == nullptr && sprite->lightlist == nullptr &&
		sprite->topclip == LARGE_VALUE && sprite->bottomclip == -LARGE_VALUE && screen->BuffersArePersistent() &&
		WorldModeActive(Level) && sprite->RenderStyle.BlendOp == STYLEOP_Add &&
		sprite->RenderStyle.DestAlpha == STYLEALPHA_InvSrc && !(sprite->RenderStyle.Flags & STYLEF_FadeToBlack) &&
		(sprite->particle != nullptr ? sprite->Colormap.Desaturation == 0 &&
			(r_particle_lit || !(Level->HasDynamicLights && !isFullbrightScene() && !sprite->fullbright && gl_light_particles)) :
			sprite->record != nullptr || (sprite->actor != nullptr && (sprite->actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)))
	{
		list = sprite->particle != nullptr ? GLDL_PARTICLES : GLDL_ALPHASPRITES;
	}
	else if (r_particle_addpass && r_sprite_transbuckets && translucent && sprite->particle != nullptr && sprite->modelframe == nullptr &&
		sprite->lightlist == nullptr && sprite->topclip == LARGE_VALUE && sprite->bottomclip == -LARGE_VALUE &&
		screen->BuffersArePersistent() && WorldModeActive(Level) && sprite->RenderStyle.BlendOp == STYLEOP_Add &&
		sprite->RenderStyle.DestAlpha == STYLEALPHA_One && !(sprite->RenderStyle.Flags & STYLEF_FadeToBlack) &&
		sprite->Colormap.Desaturation == 0 &&
		(r_particle_lit || !(Level->HasDynamicLights && !isFullbrightScene() && !sprite->fullbright && gl_light_particles)))
	{
		list = GLDL_ADDPARTICLES;
	}
	// [Karto] A full-alpha alpha-style particle is opaque, so the depth test orders it like a solid sprite; the bucket
	// pass draws it in the solid sprites' place with depth writes on.
	else if (r_particle_solidpass && r_sprite_transbuckets && !translucent && sprite->particle != nullptr &&
		sprite->modelframe == nullptr && sprite->lightlist == nullptr && sprite->topclip == LARGE_VALUE &&
		sprite->bottomclip == -LARGE_VALUE && screen->BuffersArePersistent() && WorldModeActive(Level) &&
		sprite->RenderStyle.BlendOp == STYLEOP_Add && sprite->RenderStyle.DestAlpha == STYLEALPHA_InvSrc &&
		!(sprite->RenderStyle.Flags & STYLEF_FadeToBlack) && sprite->Colormap.Desaturation == 0 &&
		(r_particle_lit || !(Level->HasDynamicLights && !isFullbrightScene() && !sprite->fullbright && gl_light_particles)))
	{
		list = GLDL_SOLIDPARTICLES;
	}
	else if (r_sprite_opaquepass && !translucent && sprite->modelframe == nullptr && (sprite->record != nullptr ||
		(sprite->actor != nullptr && (sprite->actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)))
	{
		list = GLDL_SOLIDSPRITES;
	}
	// [BB] Allow models to be drawn in the GLDL_TRANSLUCENT pass.
	else if (translucent || sprite->actor == nullptr || (!sprite->modelframe && (sprite->actor->renderflags & RF_SPRITETYPEMASK) != RF_WALLSPRITE))
	{
		list = GLDL_TRANSLUCENT;
	}
	else
	{
		list = GLDL_MODELS;
	}

	if (SpriteDumpFile) SpriteDumpWrite(this, sprite, list);
	auto newsprt = drawlists[list].NewSprite();
	*newsprt = *sprite;
}
