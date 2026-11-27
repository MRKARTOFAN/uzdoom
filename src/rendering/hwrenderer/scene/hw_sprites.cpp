/*
** hw_sprites.cpp
**
** Sprite/Particle rendering
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "colorspace.h"
#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "doomstat.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "a_pickups.h"
#include "a_corona.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "r_data/r_vanillatrans.h"
#include "matrix.h"
#include "models.h"
#include "hw_spriterecord.h"
#include "c_dispatch.h"
#include "vectors.h"
#include "texturemanager.h"
#include "basics.h"
#include "d_net.h"

#include "hw_models.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_portal.h"
#include "flatvertices.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hw_material.h"
#include "hw_dynlightdata.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "quaternion.h"

#include "p_visualthinker.h"
#include "i_time.h"
#include <climits>

extern TArray<spritedef_t> sprites;
extern TArray<spriteframe_t> SpriteFrames;
extern uint32_t r_renderercaps;

const float LARGE_VALUE = 1e19f;
const float MY_SQRT2    = 1.41421356237309504880; // sqrt(2)

EXTERN_CVAR(Bool, r_debug_disable_vis_filter)
EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Float, r_actorspriteshadowalpha)
EXTERN_CVAR(Float, r_actorspriteshadowfadeheight)
EXTERN_CVAR(Int, r_distance_cull_type)

//==========================================================================
//
// Sprite CVARs
//
//==========================================================================

CVAR(Bool, gl_usecolorblending, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_sprite_blend, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, r_sprite_batch, true, 0)
CVAR(Bool, r_sprite_gpu_check, false, 0)
CVAR(Bool, r_sprite_gpu_lights, true, 0)
CVAR(Bool, r_sprite_indexed, true, 0)
CVAR(Int, gl_spriteclip, -1, CVAR_ARCHIVE)
CVAR(Bool, r_debug_nolimitanamorphoses, false, 0)
CVAR(Float, r_spriteclipanamorphicminbias, 0.6, CVAR_ARCHIVE)
CVAR(Float, gl_sclipthreshold, 10.0, CVAR_ARCHIVE)
CVAR(Float, gl_sclipfactor, 1.8f, CVAR_ARCHIVE)
CVAR(Int, gl_particles_style, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // 0 = square, 1 = round, 2 = smooth
CVAR(Int, gl_billboard_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_faces_camera, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, hw_force_cambbpref, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, gl_fuzztype, 8, CVAR_ARCHIVE)
{
	if (self < 0 || self > 8) self = 0;
}

//==========================================================================
//
//
//
//==========================================================================

void HWSprite::DrawSprite(HWDrawInfo *di, FRenderState &state, bool translucent, bool runStart)
{
	bool additivefog = false;
	bool foglayer = false;
	int rel = fullbright ? 0 : getExtraLight();
	auto &vp = di->Viewpoint;

	if (translucent)
	{
		// The translucent pass requires special setup for the various modes.

		// for special render styles brightmaps would not look good - especially for subtractive.
		if (RenderStyle.BlendOp != STYLEOP_Add)
		{
			state.EnableBrightmap(false);
		}

		// Optionally use STYLE_ColorBlend in place of STYLE_Add for fullbright items.
		if (RenderStyle == LegacyRenderStyles[STYLE_Add] && trans > 1.f - FLT_EPSILON &&
			gl_usecolorblending && !di->isFullbrightScene() && (actor || record) &&
			fullbright && texture && !texture->GetTranslucency())
		{
			RenderStyle = LegacyRenderStyles[STYLE_ColorAdd];
		}

		state.SetRenderStyle(RenderStyle);
		state.SetTextureMode(RenderStyle);

		if (hw_styleflags == STYLEHW_NoAlphaTest)
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
		}
		else if (!texture || !texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
		else state.AlphaFunc(Alpha_Greater, 0.f);

		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			float fuzzalpha = 0.44f;
			float minalpha = 0.1f;

			// fog + fuzz don't work well without some fiddling with the alpha value!
			if (!Colormap.FadeColor.isBlack())
			{
				float dist = Dist2(vp.Pos.X, vp.Pos.Y, x, y);
				int fogd = GetFogDensity(di->Level, di->lightmode, lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

				// this value was determined by trial and error and is scale dependent!
				float factor = 0.05f + exp(-fogd * dist / 62500.f);
				fuzzalpha *= factor;
				minalpha *= factor;
			}

			state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
			state.SetColor(0.2f, 0.2f, 0.2f, fuzzalpha, Colormap.Desaturation);
			additivefog = true;
			lightlist = nullptr;	// the fuzz effect does not use the sector's light di->Level-> so splitting is not needed.
		}
		else if (RenderStyle.BlendOp == STYLEOP_Add && RenderStyle.DestAlpha == STYLEALPHA_One)
		{
			additivefog = true;
		}
	}
	else if (modelframe == nullptr)
	{
		// This still needs to set the texture mode. As blend mode it will always use GL_ONE/GL_ZERO
		state.SetTextureMode(RenderStyle);
		// [Karto] The bias keeps wall sprites in front of their wall; face sprites in the solid list match the sorted pass's no-bias draw.
		if (record == nullptr && (actor == nullptr || (actor->renderflags & RF_SPRITETYPEMASK) != RF_FACESPRITE)) state.SetDepthBias(-1, -128);
	}
	if (RenderStyle.BlendOp != STYLEOP_Shadow)
	{
		if (di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright)
		{
			if (dynlightindex == -1)	// only set if we got no light buffer index. This covers all cases where sprite lighting is used.
			{
				float out[3] = {};
				if (record != nullptr)
				{
					if (gl_light_sprites) di->GetDynSpriteLight(*record, out);
				}
				else di->GetDynSpriteLight(gl_light_sprites ? actor : nullptr, gl_light_particles ? particle : nullptr, out);
				state.SetDynLight(out[0], out[1], out[2]);
			}
		}
		sector_t *cursec = record ? record->Sector : actor ? actor->Sector : particle ? particle->subsector->sector : nullptr;
		if (cursec != nullptr)
		{
			const PalEntry finalcol = fullbright
				? ThingColor
				: ThingColor.Modulate(cursec->SpecialColors[sector_t::sprites]);

			state.SetObjectColor(finalcol);
			state.SetAddColor(cursec->AdditiveColors[sector_t::sprites] | 0xff000000);
		}
		SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, trans);
	}


	if (Colormap.FadeColor.isBlack()) foglevel = lightlevel;

	if (RenderStyle.Flags & STYLEF_FadeToBlack)
	{
		Colormap.FadeColor = 0;
		additivefog = true;
	}

	if (RenderStyle.BlendOp == STYLEOP_RevSub || RenderStyle.BlendOp == STYLEOP_Sub)
	{
		if (!modelframe)
		{
			// non-black fog with subtractive style needs special treatment
			if (!Colormap.FadeColor.isBlack() || r_distance_cull_type > 1)
			{
				foglayer = true;
				// Due to the two-layer approach we need to force an alpha test that lets everything pass
				state.AlphaFunc(Alpha_Greater, 0);
			}
		}
		else RenderStyle.BlendOp = STYLEOP_Fuzz;	// subtractive with models is not going to work.
	}

	if (!foglayer) SetFog(state, di->Level, di->lightmode, foglevel, rel, di->isFullbrightScene(), &Colormap, additivefog);
	else
	{
		state.EnableFog(false);
		state.SetFog(0, 0);
	}

	int clampmode = CLAMP_XY;

	if (texture && texture->isNoMipmap())
	{
		clampmode = CLAMP_XY_NOMIP;
	}

	uint32_t spritetype = actor? uint32_t(actor->renderflags & RF_SPRITETYPEMASK) : 0;
	if (texture) state.SetMaterial(texture, UF_Sprite, (spritetype == RF_FACESPRITE) ? CTF_Expand : 0, clampmode, translation, OverrideShader, record ? record->Class : actor ? actor->GetClass() : nullptr);
	else if (!modelframe) state.EnableTexture(false);

	//SetColor(lightlevel, rel, Colormap, trans);

	unsigned int iter = lightlist ? lightlist->Size() : 1;
	bool clipping = false;
	if (lightlist || topclip != LARGE_VALUE || bottomclip != -LARGE_VALUE)
	{
		clipping = true;
		state.EnableSplit(true);
	}

	secplane_t bottomp = { { 0, 0, -1. }, bottomclip, 1. };
	secplane_t topp = { { 0, 0, -1. }, topclip, 1. };
	for (unsigned i = 0; i < iter; i++)
	{
		if (lightlist)
		{
			// set up the light slice
			secplane_t *topplane = i == 0 ? &topp : &(*lightlist)[i].plane;
			secplane_t *lowplane = i == (*lightlist).Size() - 1 ? &bottomp : &(*lightlist)[i + 1].plane;
			int thislight = (*lightlist)[i].caster != nullptr ? RescaleLightLevel(*(*lightlist)[i].p_lightlevel) : lightlevel;
			int thisll = actor == nullptr ? thislight : (uint8_t)actor->Sector->CheckSpriteGlow(thislight, actor->InterpolatedPosition(vp.TicFrac));

			FColormap thiscm;
			thiscm.CopyFog(Colormap);
			CopyFrom3DLight(thiscm, &(*lightlist)[i]);
			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				thiscm.Decolorize();
			}

			SetColor(state, di->Level, di->lightmode, thisll, rel, di->isFullbrightScene(), thiscm, trans);
			if (!foglayer)
			{
				SetFog(state, di->Level, di->lightmode, thislight, rel, di->isFullbrightScene(), &thiscm, additivefog);
			}
			SetSplitPlanes(state, *topplane, *lowplane);
		}
		else if (clipping)
		{
			SetSplitPlanes(state, topp, bottomp);
		}

		if (!modelframe)
		{
			state.SetNormal(0, 0, 0);


			const bool batch = (runStart || (r_sprite_batch && !translucent)) && !clipping && !foglayer && (record != nullptr ||
				(actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)) && screen->BuffersArePersistent();
			if (screen->BuffersArePersistent())
			{
				CreateVertices(di, batch);
			}
			if (polyoffset)
			{
				state.SetDepthBias(-1, -128);
			}
			state.SetLightIndex(-1);
			if (!batch) state.Draw(DT_TriangleStrip, vertexindex, 4);
			else if (polyoffset) state.Draw(DT_TriangleFan, vertexindex, 4);
			else state.DrawBatchedQuad(vertexindex);

			if (foglayer)
			{
				// If we get here we know that we have colored fog and no fixed colormap.
				SetFog(state, di->Level, di->lightmode, foglevel, rel, false, &Colormap, additivefog);
				state.SetTextureMode(TM_FOGLAYER);
				state.SetRenderStyle(STYLE_Translucent);
				state.Draw(DT_TriangleStrip, vertexindex, 4);
				state.SetTextureMode(TM_NORMAL);
			}
		}
		else
		{
			if (actor && di->Level->LightProbes.Size() > 0)
			{
				LightProbe* probe = FindLightProbe(di->Level, actor->X(), actor->Y(), actor->Center());
				if (probe)
					state.SetDynLight(probe->Red, probe->Green, probe->Blue);
			}

			if(actor && (actor->flags9 & MF9_DECOUPLEDANIMATIONS))
			{
				IFVIRTUALPTR(actor, AActor, AnimateBones)
				{
					CallVM<void>(func, actor, di->Viewpoint.TicFrac);
				}
			}

			if (ModelCensusArmed) ModelCensusActor(this, di, translucent, clipping);
			FHWModelRenderer renderer(di, state, dynlightindex);
			RenderModel(&renderer, x, y, z, modelframe, actor, Net_ModifyObjectFrac(actor, di->Viewpoint.TicFrac));
			state.SetVertexBuffer(screen->mVertexData);
		}
	}

	if (runStart) return;

	if (clipping)
	{
		state.EnableSplit(false);
	}

	if (translucent)
	{
		state.EnableBrightmap(true);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
		{
			state.ClearDepthBias();
		}
	}
	else if (modelframe == nullptr)
	{
		state.ClearDepthBias();
	}

	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableTexture(true);
	state.SetDynLight(0, 0, 0);
}

void HWSprite::DrawRunRepeat(HWDrawInfo *di, FRenderState &state)
{
	CreateVertices(di, true);
	if (polyoffset)
	{
		state.SetDepthBias(-1, -128);
		state.Draw(DT_TriangleFan, vertexindex, 4);
	}
	else state.DrawBatchedQuadSameState(vertexindex);
}

// [Karto] What DrawSprite's end restores for a translucent face sprite without clipping, the only kind the additive pass takes.
void HWSprite::FinishAdditiveRun(FRenderState &state)
{
	state.EnableBrightmap(true);
	state.SetRenderStyle(STYLE_Translucent);
	state.SetTextureMode(TM_NORMAL);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableTexture(true);
	state.SetDynLight(0, 0, 0);
}

void HWSprite::FinishSolidRun(FRenderState &state)
{
	state.ClearDepthBias();
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableTexture(true);
	state.SetDynLight(0, 0, 0);
}

void HWSpriteRunKey::Set(HWDrawInfo *di, const HWSprite &s)
{
	texture = s.texture;
	cls = s.record ? (const void*)s.record->Class : (const void*)s.actor->GetClass();
	Colormap = s.Colormap;
	lightlevel = s.lightlevel;
	OverrideShader = s.OverrideShader;
	translation = s.translation.index();
	style = s.RenderStyle.AsDWORD;
	thingColor = s.ThingColor.d;
	sector_t *cursec = s.record ? s.record->Sector : s.actor->Sector;
	hasSector = cursec != nullptr;
	sectorColor = hasSector ? cursec->SpecialColors[sector_t::sprites].d : 0;
	sectorAdd = hasSector ? cursec->AdditiveColors[sector_t::sprites].d : 0;
	trans = s.trans;
	foglevel = s.foglevel;
	hw_styleflags = s.hw_styleflags;
	fullbright = s.fullbright;
	fromRecord = s.record != nullptr;
	dynlight = di->Level->HasDynamicLights && !di->isFullbrightScene() && !s.fullbright;
}

bool HWSpriteRunKey::operator==(const HWSpriteRunKey &o) const
{
	return texture == o.texture && cls == o.cls && memcmp(&Colormap, &o.Colormap, sizeof(Colormap)) == 0 &&
		lightlevel == o.lightlevel && OverrideShader == o.OverrideShader && translation == o.translation && style == o.style &&
		thingColor == o.thingColor && hasSector == o.hasSector && sectorColor == o.sectorColor && sectorAdd == o.sectorAdd &&
		trans == o.trans && foglevel == o.foglevel && hw_styleflags == o.hw_styleflags && fullbright == o.fullbright &&
		fromRecord == o.fromRecord && dynlight == o.dynlight;
}

//==========================================================================
//
//
//
//==========================================================================

static void SpriteOffsetBasis(const FRotator *HW, bool XYBillboard, FVector3& side, FVector3& up)
{
	FAngle zero = FAngle::fromDeg(0);
	FAngle pitch = (XYBillboard) ? HW->Pitch : zero;
	FAngle yaw = FAngle::fromDeg(270.) - HW->Yaw;

	FQuaternion quat = FQuaternion::FromAngles(yaw, pitch, zero);
	FVector3 sideVec = quat * FVector3(0, 1, 0);
	FVector3 upVec = quat * FVector3(0, 0, 1);
	side = FVector3(sideVec.X, sideVec.Z, sideVec.Y);
	up = FVector3(upVec.X, upVec.Z, upVec.Y);
}

void HandleSpriteOffsets(Matrix3x4 *mat, const FRotator *HW, FVector2 *offset, bool XYBillboard)
{
	FVector3 side, up;
	SpriteOffsetBasis(HW, XYBillboard, side, up);
	FVector3 res = side * offset->X + up * offset->Y;
	mat->Translate(res.X, res.Y, res.Z);
}

bool HWSprite::CalculateVertices(HWDrawInfo* di, FVector3* v, DVector3* vp)
{
	float pixelstretch = di->Level->pixelstretch;

	FVector3 center = FVector3((x1 + x2) * 0.5, (y1 + y2) * 0.5, (z1 + z2) * 0.5);
	const auto& HWAngles = di->Viewpoint.HWAngles;
	Matrix3x4 mat;
	if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
	{
		// [MC] Rotate around the center or offsets given to the sprites.
		// Counteract any existing rotations, then rotate the angle.
		// Tilt the actor up or down based on pitch (increase 'somersaults' forward).
		// Then counteract the roll and DO A BARREL ROLL.

		mat.MakeIdentity();
		FAngle pitch = FAngle::fromDeg(-Angles.Pitch.Degrees());
		pitch.Normalized180();

		mat.Translate(x, z, y);
		mat.Rotate(0, 1, 0, 270. - Angles.Yaw.Degrees());
		mat.Rotate(1, 0, 0, pitch.Degrees());

		if (actor->renderflags & RF_ROLLCENTER)
		{
			mat.Translate(center.X - x, 0, center.Y - y);
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-center.X, -z, -center.Y);
		}
		else
		{
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-x, -z, -y);
		}
		v[0] = mat * FVector3(x2, z, y2);
		v[1] = mat * FVector3(x1, z, y2);
		v[2] = mat * FVector3(x2, z, y1);
		v[3] = mat * FVector3(x1, z, y1);

		return true;
	}

	// [DVR] +ANGLEDROLL
	const bool AngledRoll = (actor != nullptr && actor->renderflags2 & RF2_ANGLEDROLL);

	// [BB] Billboard stuff
	const bool drawWithXYBillboard = ((particle && (gl_billboard_particles || gl_billboard_mode == 1) && !(particle->flags & SPF_NO_XY_BILLBOARD))
		|| (actor && !(actor->renderflags & RF_FORCEYBILLBOARD) && (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD))
		|| (record && !(record->renderflags & RF_FORCEYBILLBOARD) && (gl_billboard_mode == 1 || record->renderflags & RF_FORCEXYBILLBOARD))) && !AngledRoll;

	bool drawBillboardFacingCamera = gl_billboard_faces_camera;
	if (!hw_force_cambbpref)
	{
		if (actor)
		{
			if (actor->renderflags2 & RF2_BILLBOARDNOFACECAMERA)
				drawBillboardFacingCamera = false;
			else if (actor->renderflags2 & RF2_BILLBOARDFACECAMERA)
				drawBillboardFacingCamera = true;
		}
		else if (particle && particle->texture.isValid())
		{
			if (particle->flags & SPF_NOFACECAMERA)
				drawBillboardFacingCamera = false;
			else if (particle->flags & SPF_FACECAMERA)
				drawBillboardFacingCamera = true;
		}
	}

	// [Nash] has +ROLLSPRITE
	const bool drawRollSpriteActor = (actor != nullptr && actor->renderflags & RF_ROLLSPRITE) || (record != nullptr && record->renderflags & RF_ROLLSPRITE);
	const bool drawRollParticle = (particle != nullptr && particle->flags & SPF_ROLL);
	const bool doRoll = (drawRollSpriteActor || drawRollParticle);

	// [fgsfds] check sprite type mask
	uint32_t spritetype = (uint32_t)-1;
	if (actor != nullptr) spritetype = actor->renderflags & RF_SPRITETYPEMASK;

	// [Nash] is a flat sprite
	const bool isWallSprite = (actor != nullptr) && (spritetype == RF_WALLSPRITE);
	const bool useOffsets = (record != nullptr && !(record->renderflags & RF_ROLLCENTER)) || ((actor != nullptr) && !(actor->renderflags & RF_ROLLCENTER)) || (particle && !(particle->flags & SPF_ROLLCENTER));

	FVector2 offset = FVector2( offx, offy );
	float xx = -center.X + x;
	float yy = -center.Y + y;
	float zz = -center.Z + z;
	// [Nash] check for special sprite drawing modes
	if (drawWithXYBillboard || drawBillboardFacingCamera || isWallSprite)
	{
		mat.MakeIdentity();
		mat.Translate(center.X, center.Z, center.Y); // move to sprite center
		mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio

		// [MC] Sprite offsets.
		if (!offset.isZero())
			HandleSpriteOffsets(&mat, &HWAngles, &offset, true);

		// Order of rotations matters. Perform yaw rotation (Y, face camera) before pitch (X, tilt up/down).
		if (drawBillboardFacingCamera && !isWallSprite)
		{
			// [CMB] Rotate relative to camera XY position, not just camera direction,
			// which is nicer in VR
			float xrel = center.X - vp->X;
			float yrel = center.Y - vp->Y;
			float absAngleDeg = atan2(-yrel, xrel) * (180 / M_PI);
			float counterRotationDeg = 270. - HWAngles.Yaw.Degrees(); // counteracts existing sprite rotation
			float relAngleDeg = counterRotationDeg + absAngleDeg;

			mat.Rotate(0, 1, 0, relAngleDeg);
		}

		// [fgsfds] calculate yaw vectors
		float rollDegrees = doRoll ? Angles.Roll.Degrees() : 0;
		float angleDeg = (FAngle::fromDeg(270.) - HWAngles.Yaw).Degrees();

		// [fgsfds] Rotate the sprite about the sight vector (roll)
		if (isWallSprite)
		{
			mat.Rotate(0, 1, 0, 0);
			if (drawRollSpriteActor)
			{

				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Rotate(float_fastcosdeg(Angles.Yaw.Degrees()), 0, float_fastsindeg(Angles.Yaw.Degrees()), rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
			}
		}
		else if (doRoll)
		{
			if (useOffsets) mat.Translate(xx, zz, yy);
			if (drawWithXYBillboard)
			{
				mat.Rotate(-float_fastsindeg(angleDeg), 0, float_fastcosdeg(angleDeg), -HWAngles.Pitch.Degrees());
			}
			mat.Rotate(float_fastcosdeg(angleDeg), 0, float_fastsindeg(angleDeg), rollDegrees);
			if (useOffsets) mat.Translate(-xx, -zz, -yy);
		}
		else if (drawWithXYBillboard)
		{
			// Rotate the sprite about the vector starting at the center of the sprite
			// triangle strip and with direction orthogonal to where the player is looking
			// in the x/y plane.
			mat.Rotate(-float_fastsindeg(angleDeg), 0, float_fastcosdeg(angleDeg), -HWAngles.Pitch.Degrees());
		}

		mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
		mat.Translate(-center.X, -center.Z, -center.Y); // retreat from sprite center

		v[0] = mat * FVector3(x1, z1, y1);
		v[1] = mat * FVector3(x2, z1, y2);
		v[2] = mat * FVector3(x1, z2, y1);
		v[3] = mat * FVector3(x2, z2, y2);
	}
	else // traditional "Y" billboard mode
	{
		if (doRoll || !offset.isZero() || (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES)))
		{
			mat.MakeIdentity();

			if (!offset.isZero())
				HandleSpriteOffsets(&mat, &HWAngles, &offset, false);

			if (doRoll)
			{
				// Compute center of sprite
				float angleDeg = (FAngle::fromDeg(270.) - HWAngles.Yaw).Degrees();
				float rollDegrees = Angles.Roll.Degrees();
				float pitchDegrees = 0.0;

				if (AngledRoll)
				{
					rollDegrees = fmodf(rollDegrees, 360.0f);
					DAngle ang = di->Viewpoint.Angles.Yaw + actor->Angles.Yaw + actor->AngledRollOffset - DAngle::fromDeg(90.0);
					// [Karto] Upstream rotates an angled-roll sprite about the view/actor angle, not the outer 270 - yaw one.
					angleDeg = ang.Degrees();
					FVector3 relPos = center - FVector3(di->Viewpoint.Pos);
					if (useOffsets) relPos += FVector3(xx, yy, zz);

					Matrix3x4 rolltilt;
					rolltilt.MakeIdentity();
					ang = actor->Angles.Yaw + actor->AngledRollOffset;
					rolltilt.Rotate(float_fastcosdeg(ang.Degrees()), float_fastsindeg(ang.Degrees()), 0.0, -rollDegrees);
					pitchDegrees = 270.0 - DVector3(rolltilt * relPos).Angle().Degrees();
				}

				mat.Translate(center.X, center.Z, center.Y);
				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio
				if (AngledRoll) mat.Rotate(0.0, 1.0, 0.0, -HWAngles.Yaw.Degrees()); // Cancel regular Y-billboarding
				mat.Rotate(float_fastcosdeg(angleDeg), 0, float_fastsindeg(angleDeg), rollDegrees);
				if (AngledRoll) mat.Rotate(0.0, 1.0, 0.0, pitchDegrees); // New Y-billboarding about rolled z-axis
				mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			if (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES) && di->Viewpoint.bDoOrtho)
			{
				float angleDeg = (FAngle::fromDeg(270.) - HWAngles.Yaw).Degrees();
				mat.Translate(center.X, center.Z, center.Y);
				mat.Translate(0.0, z2 - center.Z, 0.0);
				mat.Rotate(-float_fastsindeg(angleDeg), 0, float_fastcosdeg(angleDeg), -actor->isotheta);
				mat.Translate(0.0, center.Z - z2, 0.0);
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			v[0] = mat * FVector3(x1, z1, y1);
			v[1] = mat * FVector3(x2, z1, y2);
			v[2] = mat * FVector3(x1, z2, y1);
			v[3] = mat * FVector3(x2, z2, y2);

		}
		else
		{
			v[0] = FVector3(x1, z1, y1);
			v[1] = FVector3(x2, z1, y2);
			v[2] = FVector3(x1, z2, y1);
			v[3] = FVector3(x2, z2, y2);
		}

	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

inline void HWSprite::PutSprite(HWDrawInfo *di, bool translucent)
{
	// That's a lot of checks...
	if (modelframe && !modelframe->isVoxel && !(modelframeflags & MDL_NOPERPIXELLIGHTING) && RenderStyle.BlendOp != STYLEOP_Shadow
		&& gl_light_sprites && di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright
		&& actor && !(actor->renderflags2 & RF2_NODYNAMICLIGHTING))
	{
		hw_GetDynModelLight(actor, lightdata);
		dynlightindex = screen->mLights->UploadLights(lightdata);
	}
	else
		dynlightindex = -1;

	vertexindex = -1;
	if (!screen->BuffersArePersistent())
	{
		CreateVertices(di);
	}
	di->AddSprite(this, translucent);
}

//==========================================================================
//
//
//
//==========================================================================

void HWSprite::CreateVertices(HWDrawInfo *di, bool fan)
{
	if (modelframe == nullptr)
	{
		FVector3 v[4];
		polyoffset = CalculateVertices(di, v, &di->Viewpoint.Pos);
		auto vert = screen->mVertexData->AllocVertices(4);
		auto vp = vert.first;
		vertexindex = vert.second;

		vp[0].Set(v[0][0], v[0][1], v[0][2], ul, vt);
		vp[1].Set(v[1][0], v[1][1], v[1][2], ur, vt);
		// [Karto] Fan order (batched quads share a fan-to-triangles index buffer) swaps the bottom corners of strip order.
		const int bl = fan ? 3 : 2, br = fan ? 2 : 3;
		vp[bl].Set(v[2][0], v[2][1], v[2][2], ul, vb);
		vp[br].Set(v[3][0], v[3][1], v[3][2], ur, vb);
	}

}


//==========================================================================
//
//
//
//==========================================================================

void HWSprite::SplitSprite(HWDrawInfo *di, sector_t * frontsector, bool translucent)
{
	HWSprite copySprite;
	double lightbottom;
	unsigned int i;
	bool put=false;
	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	for(i=0;i<lightlist.Size();i++)
	{
		// Particles don't go through here so we can safely assume that actor is not nullptr
		if (i<lightlist.Size()-1) lightbottom=lightlist[i+1].plane.ZatPoint(actor);
		else lightbottom=frontsector->floorplane.ZatPoint(actor);

		if (lightbottom<z2) lightbottom=z2;

		if (lightbottom<z1)
		{
			copySprite=*this;
			copySprite.lightlevel = RescaleLightLevel(*lightlist[i].p_lightlevel);
			copySprite.Colormap.CopyLight(lightlist[i].extra_colormap);

			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				copySprite.Colormap.Decolorize();
			}

			if (!ThingColor.isWhite())
			{
				copySprite.Colormap.LightColor.r = (copySprite.Colormap.LightColor.r*ThingColor.r) >> 8;
				copySprite.Colormap.LightColor.g = (copySprite.Colormap.LightColor.g*ThingColor.g) >> 8;
				copySprite.Colormap.LightColor.b = (copySprite.Colormap.LightColor.b*ThingColor.b) >> 8;
			}

			z1=copySprite.z2=lightbottom;
			vt=copySprite.vb=copySprite.vt+
				(lightbottom-copySprite.z1)*(copySprite.vb-copySprite.vt)/(z2-copySprite.z1);
			copySprite.PutSprite(di, translucent);
			put=true;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

struct FSpriteClipSource
{
	sector_t* Sector;
	DVector2 Pos;
	double floorz, ceilingz, Floorclip;
	ActorFlags flags;
	ActorFlags2 flags2;
	ActorFlags3 flags3;
	bool player, IsInventory;
};

void HWSprite::PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight)
{
	PerformSpriteClipAdjustment(FSpriteClipSource{ thing->Sector, thing->Pos().XY(), thing->floorz, thing->ceilingz, thing->Floorclip,
		thing->flags, thing->flags2, thing->flags3, thing->player != nullptr, thing->IsKindOf(NAME_Inventory) }, thingpos, spriteheight);
}

void HWSprite::PerformSpriteClipAdjustment(const FSpriteClipSource& src, const DVector2 &thingpos, float spriteheight)
{
	const FSpriteClipSource* thing = &src;
	const float NO_VAL = 100000000.0f;
	bool clipthing = (thing->player || thing->flags3&MF3_ISMONSTER || thing->IsInventory) && (thing->flags&MF_ICECORPSE || !(thing->flags&MF_CORPSE));
	bool smarterclip = !clipthing && gl_spriteclip == 3;
	if ((clipthing || gl_spriteclip > 1) && !(thing->flags2 & MF2_FLOATBOB))
	{

		float btm = NO_VAL;
		float top = -NO_VAL;
		extsector_t::xfloor &x = thing->Sector->e->XFloor;

		if (x.ffloors.Size())
		{
			for (unsigned int i = 0; i < x.ffloors.Size(); i++)
			{
				F3DFloor * ff = x.ffloors[i];
				if (ff->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
				float floorh = ff->top.plane->ZatPoint(thingpos);
				float ceilingh = ff->bottom.plane->ZatPoint(thingpos);
				if (floorh == thing->floorz)
				{
					btm = floorh;
				}
				if (ceilingh == thing->ceilingz)
				{
					top = ceilingh;
				}
				if (btm != NO_VAL && top != -NO_VAL)
				{
					break;
				}
			}
		}
		else if (thing->Sector->GetHeightSec())
		{
			if (thing->flags2&MF2_ONMOBJ && thing->floorz ==
				thing->Sector->heightsec->floorplane.ZatPoint(thingpos))
			{
				btm = thing->floorz;
				top = thing->ceilingz;
			}
		}
		if (btm == NO_VAL)
			btm = thing->Sector->floorplane.ZatPoint(thing->Pos) - thing->Floorclip;
		if (top == NO_VAL)
			top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		// +/-1 to account for the one pixel empty frame around the sprite.
		float diffb = (z2+1) - btm;
		float difft = (z1-1) - top;
		if (diffb >= 0 /*|| !gl_sprite_clip_to_floor*/) diffb = 0;
		// Adjust sprites clipping into ceiling and adjust clipping adjustment for tall graphics
		if (smarterclip)
		{
			// Reduce slightly clipping adjustment of corpses
			if (thing->flags & MF_CORPSE || spriteheight > fabs(diffb))
			{
				float ratio = clamp<float>((fabs(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
				diffb *= ratio;
			}
			if (!diffb)
			{
				if (difft <= 0) difft = 0;
				if (difft >= (float)gl_sclipthreshold)
				{
					// dumb copy of the above.
					if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || difft > (float)gl_sclipthreshold)
					{
						difft = 0;
					}
				}
				if (spriteheight > fabs(difft))
				{
					float ratio = clamp<float>((fabs(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					difft *= ratio;
				}
				z2 -= difft;
				z1 -= difft;
			}
		}
		if (diffb <= (0 - (float)gl_sclipthreshold))	// such a large displacement can't be correct!
		{
			// for living monsters standing on the floor allow a little more.
			if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || diffb < (-1.8*(float)gl_sclipthreshold))
			{
				diffb = 0;
			}
		}
		z2 -= diffb;
		z1 -= diffb;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void HWSprite::Process(HWDrawInfo *di, AActor* thing, sector_t * sector, area_t in_area, int thruportal, bool isSpriteShadow)
{
	sector_t rs;
	sector_t * rendersector;

	if (thing == nullptr)
		return;

	// [ZZ] allow CustomSprite-style direct picnum specification
	bool isPicnumOverride = thing->picnum.isValid();

	// Don't waste time projecting sprites that are definitely not visible.
	if ((thing->sprite == 0 && !isPicnumOverride) || !thing->IsVisibleToPlayer() || ((thing->renderflags & RF_MASKROTATION) && !thing->IsInsideVisibleAngles()))
	{
		return;
	}

#if 0
	if (thing->IsKindOf(NAME_Corona))
	{
		di->Coronas.Push(static_cast<ACorona*>(thing));
		return;
	}
#endif

	const auto &vp = di->Viewpoint;
	// [Karto] Both sides may be published copies; identity is the live actor's.
	AActor *camera = WorldModeLiveActor(vp.camera);

	const double alpha = thing->InterpolatedAlpha(vp.TicFrac);
	if (thing->renderflags & RF_INVISIBLE || !thing->RenderStyle.IsVisible(alpha))
	{
		// [Karto] thing may be a published copy; identity is the live actor's.
		if (!(thing->flags & MF_STEALTH) || !di->isStealthVision() || WorldModeLiveActor(thing) == camera)
			return;
	}

	// check renderrequired vs ~r_rendercaps, if anything matches we don't support that feature,
	// check renderhidden vs r_rendercaps, if anything matches we do support that feature and should hide it.
	if ((!r_debug_disable_vis_filter && !!(thing->RenderRequired & ~r_renderercaps)) ||
		(!!(thing->RenderHidden & r_renderercaps)))
		return;

	int spritenum = thing->sprite;
	DVector2 sprscale = thing->InterpolatedScale(vp.TicFrac);
	if (thing->player != nullptr)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// [RH] Interpolate the sprite's position to make it look smooth
	DVector3 thingpos = thing->InterpolatedPosition(vp.TicFrac);
	if (thruportal == 1) thingpos += di->Level->Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);

	AActor *viewmaster = thing;
	if ((thing->flags8 & MF8_MASTERNOSEE) && thing->master != nullptr)
	{
		viewmaster = thing->master;
	}

	// [Nash] filter visibility in mirrors
	bool isInMirror = di->mCurrentPortal && (di->mCurrentPortal->mState->MirrorFlag > 0 || di->mCurrentPortal->mState->PlaneMirrorFlag > 0);
	if (thing->renderflags2 & RF2_INVISIBLEINMIRRORS && isInMirror)
	{
		return;
	}
	else if (thing->renderflags2 & RF2_ONLYVISIBLEINMIRRORS && !isInMirror)
	{
		return;
	}
	// Some added checks if the camera actor is not supposed to be seen. It can happen that some portal setup has this actor in view in which case it may not be skipped here
	if (WorldModeLiveActor(viewmaster) == camera && !vp.showviewer)
	{
		// [Karto] vp.camera may be the published copy while a MASTERNOSEE thing's master is the live actor the worker
		// may have moved ahead, so compare against the copy.
		viewmaster = vp.camera;
		if (vp.bForceNoViewer || (viewmaster->player && viewmaster->player->crossingPortal)) return;
		DVector3 vieworigin = viewmaster->Pos();
		if (thruportal == 1) vieworigin += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2) return;

		// Necessary in order to prevent sprite pop-ins with viewpos and models.
		auto* sec = viewmaster->Sector;
		if (sec && !sec->PortalBlocksMovement(sector_t::ceiling))
		{
			double zh = sec->GetPortalPlaneZ(sector_t::ceiling);
			double top = (viewmaster->player ? max<double>(viewmaster->player->viewz, viewmaster->Top()) + 1 : viewmaster->Top());
			if (viewmaster->Z() < zh && top >= zh)
				return;
		}
	}
	// Thing is invisible if close to the camera.
	if (viewmaster->renderflags & RF_MAYBEINVISIBLE)
	{
		DVector3 viewpos = viewmaster->InterpolatedPosition(vp.TicFrac);
		if (thruportal == 1) viewpos += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(viewpos.X - vp.Pos.X) < 32 && fabs(viewpos.Y - vp.Pos.Y) < 32) return;
	}

	modelframe = isPicnumOverride ? nullptr : FindModelFrame(thing, spritenum, thing->frame, !!(thing->flags & MF_DROPPED));
	modelframeflags = modelframe ? modelframe->getFlags(thing->modelData) : 0;

	// Too close to the camera. This doesn't look good if it is a sprite.
	if (fabs(thingpos.X - vp.Pos.X) < 2 && fabs(thingpos.Y - vp.Pos.Y) < 2
		&& vp.Pos.Z >= thingpos.Z - 2 && vp.Pos.Z <= thingpos.Z + thing->Height + 2
		&& !thing->Vel.isZero() && !modelframe) // exclude vertically moving objects from this check.
	{
		return;
	}

	// don't draw first frame of a player missile
	if (thing->flags&MF_MISSILE)
	{
		// [Karto] vp.ViewActor may be the published copy while a copy's target still points at the live actor.
		if (!(thing->flags7 & MF7_FLYCHEAT) && vp.ViewActor != nullptr && thing->target == WorldModeLiveActor(vp.ViewActor))
		{
			double speed = thing->Vel.Length();
			if (speed >= thing->target->radius / 2)
			{
				double clipdist = clamp(thing->Speed, thing->target->radius, thing->target->radius * 2);
				if ((thingpos - vp.Pos).LengthSquared() < clipdist * clipdist) return;
			}
		}
		thing->flags7 |= MF7_FLYCHEAT;	// do this only once for the very first frame, but not if it gets into range again.
	}

	if (thruportal != 2 && di->mClipPortal != nullptr)
	{
		int clipres = di->mClipPortal->ClipPoint(thingpos.XY());
		if (clipres == PClip_InFront) return;
	}
	// disabled because almost none of the actual game code is even remotely prepared for this. If desired, use the INTERPOLATE flag.
	if (thing->renderflags & RF_INTERPOLATEANGLES)
		Angles = thing->InterpolatedAngles(vp.TicFrac);
	else
		Angles = thing->Angles;

	if (sector->sectornum != thing->Sector->sectornum && !thruportal)
	{
		// This cannot create a copy in the fake sector cache because it'd interfere with the main thread, so provide a local buffer for the copy.
		// Adding synchronization for this one case would cost more than it might save if the result here could be cached.
		rendersector = hw_FakeFlat(thing->Sector, in_area, false, &rs);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	uint32_t spritetype = (thing->renderflags & RF_SPRITETYPEMASK);
	x = thingpos.X + thing->WorldOffset.X;
	z = thingpos.Z + thing->WorldOffset.Z;
	y = thingpos.Y + thing->WorldOffset.Y;
	if (spritetype == RF_FACESPRITE) z -= thing->Floorclip; // wall and flat sprites are to be considered di->Level-> geometry so this may not apply.

	// snap shadow Z to the floor
	if (isSpriteShadow)
	{
		z = thing->floorz;
	}
	// [RH] Make floatbobbing a renderer-only effect.
	else
	{
		float fz = thing->GetBobOffsetAt(WorldModeFrameTic(thing->Level), Net_ModifyObjectFrac(thing, vp.TicFrac));
		z += fz;
	}

	// don't bother drawing sprite shadows if this is a model (it will never look right)
	if (modelframe && isSpriteShadow)
	{
		return;
	}
	if (!modelframe)
	{
		bool mirror = false;
		bool backfaced = false;
		DAngle ang = (thingpos - vp.Pos).Angle();
		if (di->Viewpoint.bDoOrtho) ang = vp.Angles.Yaw;
		else if (actor && thing->renderflags2 & RF2_ANGLEDROLL)
		{
			Matrix3x4 rolltilt;
			rolltilt.MakeIdentity();
			DAngle tempang = thing->Angles.Yaw + thing->AngledRollOffset;
			rolltilt.Rotate(tempang.Cos(), tempang.Sin(), 0.0, -thing->Angles.Roll.Degrees());
			tempang = DVector3(rolltilt * FVector3(thingpos - vp.Pos)).Angle();
			backfaced = fabs((ang - tempang).Degrees()) > 90.0;
			if (backfaced) // (ang - tempang).Cos() < 0.0)
			{
				ang = DAngle::fromDeg(180.0) - ang;
			}
		}
		FTextureID patch;
		// [ZZ] add direct picnum override
		if (isPicnumOverride)
		{
			// Animate picnum overrides.
			auto tex = TexMan.GetGameTexture(thing->picnum, true);
			if (tex == nullptr) return;

			if (tex->GetRotations() != 0xFFFF)
			{
				// choose a different rotation based on player view
				spriteframe_t* sprframe = &SpriteFrames[tex->GetRotations()];
				DAngle sprang = thing->GetSpriteAngle(ang, vp.TicFrac);
				angle_t rot;
				if (sprframe->Texture[0] == sprframe->Texture[1])
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
				}
				else
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
				}
				auto picnum = sprframe->Texture[rot];
				if (sprframe->Flip & (1 << rot))
				{
					mirror = true;
				}
			}

			patch =  tex->GetID();
		}
		else
		{
			DAngle sprangle;
			int rot;
			if (!(thing->renderflags & RF_FLATSPRITE) || thing->flags7 & MF7_SPRITEANGLE)
			{
				sprangle = thing->GetSpriteAngle(ang, vp.TicFrac);
				rot = -1;
			}
			else
			{
				// Flat sprites cannot rotate in a predictable manner.
				sprangle = nullAngle;
				rot = 0;
			}
			patch = sprites[spritenum].GetSpriteFrame(thing->frame, rot, sprangle, &mirror, !!(thing->renderflags & RF_SPRITEFLIP));
		}

		if (!patch.isValid()) return;
		int type = thing->renderflags & RF_SPRITETYPEMASK;
		auto tex = TexMan.GetGameTexture(patch, false);
		if (!tex || !tex->isValid()) return;
		auto& spi = tex->GetSpritePositioning(type == RF_FACESPRITE);

		offx = (float)thing->GetSpriteOffset(false);
		offy = (float)thing->GetSpriteOffset(true);

		vt = spi.GetSpriteVT();
		vb = spi.GetSpriteVB();
		if (thing->renderflags & RF_YFLIP) std::swap(vt, vb);

		auto r = spi.GetSpriteRect();

		// [SP] SpriteFlip
		if (thing->renderflags & RF_SPRITEFLIP)
			thing->renderflags ^= RF_XFLIP;

		if (mirror ^ !!(thing->renderflags & RF_XFLIP))
		{
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
			ul = spi.GetSpriteUL();
			ur = spi.GetSpriteUR();
		}
		else
		{
			ul = spi.GetSpriteUR();
			ur = spi.GetSpriteUL();
		}

		texture = tex;
		if (!texture || !texture->isValid())
			return;

		// Canvas textures are stored upside down
		if (!isPicnumOverride && texture && texture->isHardwareCanvas()) std::swap(vt, vb);

		if (thing->renderflags & RF_SPRITEFLIP) // [SP] Flip back
			thing->renderflags ^= RF_XFLIP;

		// If sprite is isometric, do both vertical scaling and partial rotation to face the camera to compensate for Y-billboarding.
		// Using just rotation (about z=0) might cause tall+slender (high aspect ratio) sprites to clip out of collision box
		// at the top and clip into whatever is behind them from the viewpoint's perspective. - [DVR]
		thing->isoscaleY = 1.0;
		thing->isotheta = vp.HWAngles.Pitch.Degrees();
		if (thing->renderflags2 & RF2_ISOMETRICSPRITES)
		{
			float floordist = thing->radius * vp.floordistfact;
			floordist -= 0.5 * r.width * vp.cotfloor;
			float sineisotheta = floordist / r.height;
			double scl = g_sqrt( 1.0 + sineisotheta * sineisotheta - 2.0 * vp.PitchSin * sineisotheta );
			if ((thing->radius > 0.0) && (scl > fabs(vp.PitchCos)))
			{
				thing->isoscaleY = scl / ( fabs(vp.PitchCos) > 0.01 ? fabs(vp.PitchCos) : 0.01 );
				thing->isotheta = 180.0 * asin( sineisotheta / thing->isoscaleY ) / M_PI;
			}
		}

		r.Scale(sprscale.X, isSpriteShadow ? sprscale.Y * 0.15 * thing->isoscaleY : sprscale.Y * thing->isoscaleY);

		if (((thing->renderflags & RF_ROLLSPRITE) || (thing->renderflags2 & RF2_SQUAREPIXELS)) && !(thing->renderflags2 & RF2_STRETCHPIXELS))
		{
			double ps = di->Level->pixelstretch;
			double mult = 1.0 / sqrt(ps); // shrink slightly
			r.Scale(mult * ps, mult);
		}

		float rightfac = -r.left;
		float leftfac = rightfac - r.width;
		z1 = z - r.top;
		z2 = z1 - r.height;

		float spriteheight = sprscale.Y * r.height * thing->isoscaleY;

		// Tests show that this doesn't look good for many decorations and corpses
		if (spriteheight > 0 && gl_spriteclip > 0 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
		{
			PerformSpriteClipAdjustment(thing, thingpos.XY(), spriteheight);
		}

		switch (spritetype)
		{
		case RF_FACESPRITE:
		{
			float viewvecX = vp.ViewVector.X;
			float viewvecY = vp.ViewVector.Y;

			x1 = x - viewvecY*leftfac;
			x2 = x - viewvecY*rightfac;
			y1 = y + viewvecX*leftfac;
			y2 = y + viewvecX*rightfac;
			if (thing->renderflags2 & RF2_ISOMETRICSPRITES) // If sprites are drawn from an isometric perspective
			{
				x1 -= viewvecX * thing->radius * MY_SQRT2;
				x2 -= viewvecX * thing->radius * MY_SQRT2;
				y1 -= viewvecY * thing->radius * MY_SQRT2;
				y2 -= viewvecY * thing->radius * MY_SQRT2;
			}
			break;
		}
		case RF_FLATSPRITE:
		{
			float bottomfac = -r.top;
			float topfac = bottomfac - r.height;

			x1 = x + leftfac;
			x2 = x + rightfac;
			y1 = y - topfac;
			y2 = y - bottomfac;
			// [MC] Counteract in case of any potential problems. Tests so far haven't
			// shown any outstanding issues but that doesn't mean they won't appear later
			// when more features are added.
			z1 += offy;
			z2 += offy;
			break;
		}
		case RF_WALLSPRITE:
		{
			float viewvecX = Angles.Yaw.Cos();
			float viewvecY = Angles.Yaw.Sin();

			x1 = x + viewvecY*leftfac;
			x2 = x + viewvecY*rightfac;
			y1 = y - viewvecX*leftfac;
			y2 = y - viewvecX*rightfac;
			break;
		}
		}
	}
	else
	{
		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		texture = nullptr;
	}

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);
	if(thing->renderflags2 & RF2_ISOMETRICSPRITES) depth = depth * vp.PitchCos - vp.PitchSin * z2; // Helps with stacking actors with small xy offsets
	if(actor && (thing->renderflags2 & RF2_ANGLEDROLL) && !(thing->renderflags & RF_ROLLCENTER))
	{
		double rollsin = thing->Angles.Roll.Sin();
		DAngle tempang2 = thing->Angles.Yaw + thing->AngledRollOffset + DAngle::fromDeg(rollsin < 0.0
																						? 90.0 : -90.0);
		double r2 = 0.5 * fabs((z2 - z1) * rollsin);
		depth = (float)((x + r2 * tempang2.Cos() - vp.Pos.X) * vp.TanCos
						+ (y + r2 * tempang2.Sin() - vp.Pos.Y) * vp.TanSin);
	}
	if (isSpriteShadow) depth += EQUAL_EPSILON; // always sort shadows behind the sprite.

	if (gl_spriteclip == -1 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE) // perform anamorphosis
	{
		float minbias = r_spriteclipanamorphicminbias;
		minbias = clamp(minbias, 0.3f, 1.0f);

		float btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		float top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		float vbtm = thing->Sector->floorplane.ZatPoint(vp.Pos);
		float vtop = thing->Sector->ceilingplane.ZatPoint(vp.Pos);

		float vpx = vp.Pos.X;
		float vpy = vp.Pos.Y;
		float vpz = vp.Pos.Z;

		float tpx = thingpos.X;
		float tpy = thingpos.Y;
		float tpz = thingpos.Z;

		if (!(r_debug_nolimitanamorphoses))
		{
			// this should help prevent clipping through walls ...
			float objradiusbias = 1.f - thing->radius / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
			minbias = max(minbias, objradiusbias);
		}

		float bintersect, tintersect;
		if (z2 < vpz && vbtm < vpz)
			bintersect = min((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
		else
			bintersect = 1.0;

		if (z1 > vpz && vtop > vpz)
			tintersect = min((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
		else
			tintersect = 1.0;

		if (thing->waterlevel >= 1 && thing->waterlevel <= 2)
			bintersect = tintersect = 1.0f;

		float spbias = clamp(min(bintersect, tintersect), minbias, 1.0f);
		float vpbias = 1.0 - spbias;
		x1 = x1 * spbias + vpx * vpbias;
		y1 = y1 * spbias + vpy * vpbias;
		z1 = z1 * spbias + vpz * vpbias;
		x2 = x2 * spbias + vpx * vpbias;
		y2 = y2 * spbias + vpy * vpbias;
		z2 = z2 * spbias + vpz * vpbias;
	}

	// light calculation

	bool enhancedvision = false;

	// allow disabling of the fullbright flag by a brightmap definition
	// (e.g. to do the gun flashes of Doom's zombies correctly.
	fullbright = (thing->flags5 & MF5_BRIGHT) ||
		((thing->renderflags & RF_FULLBRIGHT) && (!texture || !texture->isFullbrightDisabled()));

	if (fullbright)	lightlevel = 255;
	else lightlevel = RescaleLightLevel(thing->GetLightLevel(rendersector));

	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255); // this *must* use the sector's light level or the fog will just look bad.

	lightlevel = rendersector->CheckSpriteGlow(lightlevel, thingpos);

	ThingColor = (thing->RenderStyle.Flags & STYLEF_ColorIsFixed) ? thing->fillcolor : Color::White;
	ThingColor.a = 255;
	RenderStyle = thing->RenderStyle;

	// colormap stuff is a little more complicated here...
	if (di->isFullbrightScene())
	{
		enhancedvision = di->isStealthVision();

		Colormap.Clear();

		if (di->isNightvision())
		{
			if ((thing->IsKindOf(NAME_Inventory) || thing->flags3&MF3_ISMONSTER || thing->flags&MF_MISSILE || thing->flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &di->Level->sectors[rendersector->sectornum] || in_area != area_below)
				// under water areas keep their color for fullbright objects
			{
				// Only make the light white but keep everything else (fog, desaturation and Boom colormap.)
				Colormap.MakeWhite();
			}
			else
			{
				// Keep the color, but brighten things a bit so that a difference can be seen.
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = thing->Translation;

	OverrideShader = -1;
	trans = alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;	// trans may not be 1 here
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				// Without shaders only the standard effect is available.
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency && (thing->renderflags & RF_ZDOOMTRANS))
	{
		// [SP] "canonical transparency" - with the flip of a CVar, disable transparency for Doom objects,
		//   and disable 'additive' translucency for certain objects from other games.
		if (r_UseVanillaTransparency == 1 || AutoTrans.CheckKey(thing->GetClass()->TypeName) != nullptr)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
		trans = 1.f;

		if (!gl_sprite_blend || modelframe ||
			(thing->renderflags & (RF_FLATSPRITE | RF_WALLSPRITE)) ||
			(hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && !(thing->renderflags2 & RF2_BILLBOARDNOFACECAMERA)) ||
			thing->renderflags2 & RF2_BILLBOARDFACECAMERA))
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((texture && texture->GetTranslucency()) || (RenderStyle.Flags & STYLEF_RedIsAlpha) || (modelframe && thing->RenderStyle != DefaultRenderStyle()))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			// enhanced vision makes them more visible!
			trans = 0.5f;
			FRenderStyle rs = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
		}
		else if (thing->flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	// for sprite shadow, use a translucent stencil renderstyle
	if (isSpriteShadow)
	{
		RenderStyle = STYLE_Stencil;
		ThingColor = MAKEARGB(255, 0, 0, 0);
		// fade shadow progressively as the thing moves higher away from the floor
		if (r_actorspriteshadowfadeheight > 0.0) {
			trans *= clamp(0.0f, float(r_actorspriteshadowalpha - (thingpos.Z - thing->floorz) * (1.0 / r_actorspriteshadowfadeheight)), float(r_actorspriteshadowalpha));
		} else {
			trans *= r_actorspriteshadowalpha;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (trans == 0.0f) return;

	// end of light calculation

	actor = thing;
	index = thing->SpawnOrder;

	// sprite shadows should have a fixed index of -1 (ensuring they're drawn behind particles which have index 0)
	// sorting should be irrelevant since they're always translucent
	if (isSpriteShadow)
	{
		index = -1;
	}

	particle = nullptr;

	const bool drawWithXYBillboard = (!(actor->renderflags & RF_FORCEYBILLBOARD)
		&& (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE
		&& (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD));


	// no light splitting when:
	// 1. no lightlist
	// 2. any fixed colormap
	// 3. any bright object
	// 4. any with render style shadow (which doesn't use the sector light)
	// 5. anything with render style reverse subtract (light effect is not what would be desired here)
	if (thing->Sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)	// on old hardware we are rather limited...
		{
			lightlist = nullptr;
			if (!drawWithXYBillboard && !modelframe)
			{
				SplitSprite(di, thing->Sector, hw_styleflags != STYLEHW_Solid);
			}
		}
		else
		{
			lightlist = &thing->Sector->e->XFloor.lightlist;
		}
	}
	else
	{
		lightlist = nullptr;
	}

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

struct FSpriteInstanceGeometry
{
	FGameTexture* Texture;
	float x1, y1, z1, x2, y2, z2, ul, ur, vt, vb, depth;
	int Rotation;
	FVector3 v[4];		// the corners as CalculateVertices gives them: (x, z, y), top left, top right, bottom left, bottom right
};

static float SpriteAnamorphicBias(float z1, float z2, float btm, float top, float vbtm, float vtop, float vpx, float vpy, float vpz,
	float tpx, float tpy, double radius, bool shallowWater)
{
	float minbias = r_spriteclipanamorphicminbias;
	minbias = clamp(minbias, 0.3f, 1.0f);
	if (!(r_debug_nolimitanamorphoses))
	{
		float objradiusbias = 1.f - radius / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
		minbias = max(minbias, objradiusbias);
	}
	float bintersect, tintersect;
	if (z2 < vpz && vbtm < vpz)
		bintersect = min((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
	else
		bintersect = 1.0;
	if (z1 > vpz && vtop > vpz)
		tintersect = min((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
	else
		tintersect = 1.0;
	if (shallowWater)
		bintersect = tintersect = 1.0f;
	return clamp(min(bintersect, tintersect), minbias, 1.0f);
}

static FVector3 SpriteRotate(const FVector3& k, float deg, const FVector3& d)
{
	const float a = deg * float(M_PI / 180.0), c = cosf(a), s = sinf(a);
	return d * c + (k ^ d) * s + k * ((k | d) * (1.f - c));
}

struct FSpriteViewParams
{
	float AxisDeg, PitchDeg, PixelStretch;
	FVector3 Side[2], Up[2];	// offset directions: Y billboard, XY billboard
};

static void SpriteInstanceCorners(FVector3 v[4], const FVector3& origin, const FSpriteInstance& in, const FSpriteViewParams& p)
{
	const bool roll = (in.Flags & SIF_Roll) != 0, xy = (in.Flags & SIF_XYBillboard) != 0;
	if (!roll && !xy && in.Offset[0] == 0 && in.Offset[1] == 0) return;
	const float ar = p.AxisDeg * float(M_PI / 180.0);
	const FVector3 rollAxis(cosf(ar), 0.f, sinf(ar)), pitchAxis(-sinf(ar), 0.f, cosf(ar));
	const FVector3 centre = (v[0] + v[3]) * 0.5f;
	const FVector3 res = p.Side[xy] * in.Offset[0] + p.Up[xy] * in.Offset[1];
	for (int i = 0; i < 4; ++i)
	{
		if (xy)
		{
			FVector3 d = v[i] - centre;
			d.Y *= p.PixelStretch;
			if (roll)
			{
				const FVector3 o = (in.Flags & SIF_RollCenter) ? FVector3(0, 0, 0) : origin - centre;
				d = SpriteRotate(pitchAxis, -p.PitchDeg, SpriteRotate(rollAxis, in.Roll, d - o)) + o;
			}
			else d = SpriteRotate(pitchAxis, -p.PitchDeg, d);
			d += res;
			d.Y /= p.PixelStretch;
			v[i] = centre + d;
		}
		else if (roll)
		{
			const FVector3 pivot = (in.Flags & SIF_RollCenter) ? centre : origin;
			FVector3 d = v[i] - pivot;
			d.Y *= p.PixelStretch;
			d = SpriteRotate(rollAxis, in.Roll, d);
			d.Y /= p.PixelStretch;
			v[i] = pivot + d + res;
		}
		else v[i] += res;
	}
}

static FSpriteViewParams SpriteViewParams(HWDrawInfo* di)
{
	const auto& vp = di->Viewpoint;
	FSpriteViewParams p;
	p.AxisDeg = float((FAngle::fromDeg(270.) - vp.HWAngles.Yaw).Degrees());
	p.PitchDeg = float(vp.HWAngles.Pitch.Degrees());
	p.PixelStretch = float(di->Level->pixelstretch);
	SpriteOffsetBasis(&vp.HWAngles, false, p.Side[0], p.Up[0]);
	SpriteOffsetBasis(&vp.HWAngles, true, p.Side[1], p.Up[1]);
	return p;
}

static bool SpriteInstanceGeometry(HWDrawInfo* di, const FSpriteInstance& in, FSpriteInstanceGeometry& g)
{
	const FSpriteFrameEntry* e = WorldModeSpriteFrameEntry(in.Frame);
	if (e == nullptr || !e->Usable) return false;
	const auto& vp = di->Viewpoint;
	const float t = float(vp.TicFrac);
	float pos[3];
	for (int k = 0; k < 3; ++k) pos[k] = (in.Flags & SIF_DontInterpolate) ? in.Pos[k] : in.Prev[k] * (1.f - t) + in.Pos[k] * t;
	const float x = pos[0] + in.WorldOffset[0], y = pos[1] + in.WorldOffset[1];
	float z = pos[2] + in.WorldOffset[2] - in.Floorclip;
	if (in.Flags & SIF_FloatBob)
	{
		const float bt = float(Net_ModifyFrac((in.Flags & SIF_ClientSide) != 0, vp.TicFrac));
		z += sinf((in.BobPhase + float(WorldModeFrameTic(di->Level)) * in.BobFactor + bt) * (180.f / 32.f) * float(M_PI / 180.0)) * 8.f * in.BobStrength;
	}
	float ang = di->Viewpoint.bDoOrtho ? float(vp.Angles.Yaw.Degrees()) :
		atan2f(pos[1] - float(vp.Pos.Y), pos[0] - float(vp.Pos.X)) * float(180.0 / M_PI);
	const float sprang = (in.Flags & SIF_SpriteAngle) ? in.Angle : ang - in.Angle;
	const bool flipagain = (in.Flags & SIF_SpriteFlip) != 0;
	float a = e->SameFirst ? (flipagain ? -sprang + 360.f + 202.5f : sprang + 202.5f) : (flipagain ? -sprang + 360.f + 191.25f : sprang + 191.25f);
	a -= 360.f * floorf(a / 360.f);
	const int rot = int(a * (16.f / 360.f)) & 15;
	g.Rotation = rot;
	g.Texture = e->Texture[rot];
	const bool mirror = (e->Flip >> rot) & 1;
	g.vt = e->VT[rot];
	g.vb = e->VB[rot];
	if (in.Flags & SIF_YFlip) std::swap(g.vt, g.vb);
	FloatRect rect = e->Rect[rot];
	const bool xflip = ((in.Flags & SIF_XFlip) != 0) != ((in.Flags & SIF_SpriteFlip) != 0);
	if (mirror != xflip)
	{
		rect.left = -rect.width - rect.left;
		g.ul = e->UL[rot];
		g.ur = e->UR[rot];
	}
	else
	{
		g.ul = e->UR[rot];
		g.ur = e->UL[rot];
	}
	rect.Scale(in.Scale[0], in.Scale[1]);
	const float pixelstretch = float(di->Level->pixelstretch);
	if (in.Flags & SIF_RollScale)
	{
		const float mult = 1.f / sqrtf(pixelstretch);
		rect.Scale(mult * pixelstretch, mult);
	}
	const float rightfac = -rect.left, leftfac = rightfac - rect.width;
	g.z1 = z - rect.top;
	g.z2 = g.z1 - rect.height;
	const float spriteheight = in.Scale[1] * rect.height;
	if (spriteheight > 0 && gl_spriteclip > 0)
	{
		const bool clipthing = (in.Flags & SIF_ClipThing) != 0;
		const bool smarterclip = !clipthing && gl_spriteclip == 3;
		if ((clipthing || gl_spriteclip > 1) && !(in.Flags & SIF_FloatBob))
		{
			const float btm = in.ClipBottom;
			const float top = (in.Ceiling[3] + in.Ceiling[0] * pos[0] + in.Ceiling[1] * pos[1]) * in.Ceiling[2];
			float diffb = (g.z2 + 1) - btm;
			float difft = (g.z1 - 1) - top;
			if (diffb >= 0) diffb = 0;
			if (smarterclip)
			{
				if ((in.Flags & SIF_Corpse) || spriteheight > fabsf(diffb))
				{
					const float ratio = clamp<float>((fabsf(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					diffb *= ratio;
				}
				if (!diffb)
				{
					if (difft <= 0) difft = 0;
					if (difft >= (float)gl_sclipthreshold)
					{
						if (!(in.Flags & SIF_Monster) || (in.Flags & SIF_NoGravity) || (in.Flags & SIF_Corpse) || difft > (float)gl_sclipthreshold)
							difft = 0;
					}
					if (spriteheight > fabsf(difft))
					{
						const float ratio = clamp<float>((fabsf(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
						difft *= ratio;
					}
					g.z2 -= difft;
					g.z1 -= difft;
				}
			}
			if (diffb <= (0 - (float)gl_sclipthreshold))
			{
				if (!(in.Flags & SIF_Monster) || (in.Flags & SIF_NoGravity) || (in.Flags & SIF_Corpse) || diffb < (-1.8f * (float)gl_sclipthreshold))
					diffb = 0;
			}
			g.z2 -= diffb;
			g.z1 -= diffb;
		}
	}
	const float viewvecX = vp.ViewVector.X, viewvecY = vp.ViewVector.Y;
	g.x1 = x - viewvecY * leftfac;
	g.x2 = x - viewvecY * rightfac;
	g.y1 = y + viewvecX * leftfac;
	g.y2 = y + viewvecX * rightfac;
	g.depth = (x - float(vp.Pos.X)) * float(vp.TanCos) + (y - float(vp.Pos.Y)) * float(vp.TanSin);
	if (gl_spriteclip == -1)
	{
		const float vpx = float(vp.Pos.X), vpy = float(vp.Pos.Y), vpz = float(vp.Pos.Z);
		auto planeZ = [](const float* p, float px, float py) { return (p[3] + p[0] * px + p[1] * py) * p[2]; };
		const float spbias = SpriteAnamorphicBias(g.z1, g.z2, in.ClipBottom, planeZ(in.Ceiling, pos[0], pos[1]), planeZ(in.Floor, vpx, vpy),
			planeZ(in.Ceiling, vpx, vpy), vpx, vpy, vpz, pos[0], pos[1], in.Radius, (in.Flags & SIF_ShallowWater) != 0);
		const float vpbias = 1.0 - spbias;
		g.x1 = g.x1 * spbias + vpx * vpbias;
		g.y1 = g.y1 * spbias + vpy * vpbias;
		g.z1 = g.z1 * spbias + vpz * vpbias;
		g.x2 = g.x2 * spbias + vpx * vpbias;
		g.y2 = g.y2 * spbias + vpy * vpbias;
		g.z2 = g.z2 * spbias + vpz * vpbias;
	}
	g.v[0] = FVector3(g.x1, g.z1, g.y1);
	g.v[1] = FVector3(g.x2, g.z1, g.y2);
	g.v[2] = FVector3(g.x1, g.z2, g.y1);
	g.v[3] = FVector3(g.x2, g.z2, g.y2);
	SpriteInstanceCorners(g.v, FVector3(x, z, y), in, SpriteViewParams(di));
	return true;
}

static uint64_t SpriteGpuChecked, SpriteGpuTextureMismatch, SpriteGpuRotationMismatch, SpriteGpuGeometryMismatch;
static double SpriteGpuMaxError;

EXTERN_CVAR(Bool, r_sprite_gpu)
EXTERN_CVAR(Bool, r_sprite_records)
EXTERN_CVAR(Bool, r_sprite_opaquepass)
EXTERN_CVAR(Bool, r_worldmode_snapshot)
EXTERN_CVAR(Bool, gl_light_sprites)
EXTERN_CVAR(Int, gl_billboard_mode)
EXTERN_CVAR(Bool, gl_billboard_faces_camera)
EXTERN_CVAR(Bool, hw_force_cambbpref)

// [Karto] Main view only; any condition under which ProcessRecord would differ from the shader sends the whole scene to
// the CPU path.
bool HWDrawInfo::UseGpuSprites()
{
	if (!r_sprite_gpu || !r_sprite_records || !r_worldmode_snapshot || !r_sprite_opaquepass || gl_sprite_blend) return false;
	if (mCurrentPortal != nullptr || gl_billboard_mode == 1 || gl_billboard_faces_camera || isNightvision() || isStealthVision()) return false;
	if (Level->HasDynamicLights && gl_light_sprites && !r_sprite_gpu_lights) return false;
	return screen->RenderState()->SupportsSpriteInstances();
}

EXTERN_CVAR(Bool, r_particle_gpu)
EXTERN_CVAR(Bool, r_sprite_transbuckets)

bool HWDrawInfo::UseGpuParticles()
{
	if (!r_particle_gpu || !r_worldmode_snapshot || !r_sprite_transbuckets || mCurrentPortal != nullptr) return false;
	if (!screen->BuffersArePersistent() || !screen->RenderState()->SupportsParticleExpansion()) return false;
	const float* data;
	uint32_t count;
	uint64_t serial;
	if (!WorldModeGpuParticleData(Level, data, count, serial)) return false;
	GpuParticleSerial = serial;
	return true;
}

static uint64_t GpuSpriteScenes, GpuSpriteNoPublish, GpuSpriteDraws, GpuSpriteDeclined, GpuSpriteInstances, GpuSpriteRows;
static uint64_t GpuSpriteLightFrames, GpuSpriteLightComputed, GpuSpriteLightReused, GpuSpriteLightNS;

CCMD(sprite_gpu_stats)
{
	Printf("SPRITEGPUSTATS scenes=%llu no_publish=%llu draws=%llu declined=%llu instances=%llu rows=%llu\n", (unsigned long long)GpuSpriteScenes,
		(unsigned long long)GpuSpriteNoPublish, (unsigned long long)GpuSpriteDraws, (unsigned long long)GpuSpriteDeclined,
		(unsigned long long)GpuSpriteInstances, (unsigned long long)GpuSpriteRows);
	Printf("SPRITEGPULIGHTS frames=%llu computed=%llu reused=%llu us_per_frame=%.1f\n", (unsigned long long)GpuSpriteLightFrames,
		(unsigned long long)GpuSpriteLightComputed, (unsigned long long)GpuSpriteLightReused,
		GpuSpriteLightFrames ? double(GpuSpriteLightNS) / GpuSpriteLightFrames / 1000.0 : 0.0);
}

struct FGpuSpriteLightCache
{
	const FSpriteInstance* PublishInstances = nullptr;
	int PublishTic = INT_MIN;
	uint32_t Serial = 0;
	std::vector<uint32_t> Stamps;
	std::vector<float> Colors;

	void Begin(const FSpriteInstance* instances, int tic)
	{
		if (instances == PublishInstances && tic == PublishTic) return;
		PublishInstances = instances;
		PublishTic = tic;
		if (++Serial == 0)
		{
			std::fill(Stamps.begin(), Stamps.end(), 0u);
			Serial = 1;
		}
	}

	void Get(HWDrawInfo* di, const FSpriteInstance& in, const FSpriteAppearanceKey* keys, float* out)
	{
		out[0] = out[1] = out[2] = 0.f;
		const FSpriteRecord* record = WorldModeSpriteRecord(in.Record);
		if (record == nullptr || keys[in.Appearance].Fullbright) return;
		const size_t r = size_t(in.Record);
		if (r >= Stamps.size())
		{
			Stamps.resize(r + 1, 0u);
			Colors.resize((r + 1) * 3, 0.f);
		}
		if (Stamps[r] != Serial)
		{
			di->GetDynSpriteLight(*record, &Colors[r * 3]);
			Stamps[r] = Serial;
			++GpuSpriteLightComputed;
		}
		else ++GpuSpriteLightReused;
		memcpy(out, &Colors[r * 3], 3 * sizeof(float));
	}
} GpuSpriteLights;

// [Karto] Within a frame entry the order is the publish's, not the traversal's: only blending among overlapping
// translucent sprites of one frame differs.
CVAR(Bool, r_sprite_gpu_pertic, true, 0)
CVAR(Bool, r_sprite_gpu_lightpass, true, 0)
static struct
{
	uint32_t Serial = 0;
	bool GpuLights = false;
	std::vector<uint32_t> Order[3], App[3], Mark[3];
	std::vector<float> Light[3];
	std::vector<uint8_t> LightDone[3];
	std::vector<uint32_t> PosOf;	// instance -> position in its blend's order
	uint32_t MarkSerial = 0;
} GpuSpriteTic;

void HWDrawInfo::DrawGpuSprites(FRenderState &state, int blend)
{
	const FSpriteInstance* instances;
	const FSpriteAppearanceKey* keys;
	uint32_t count, keyCount;
	if (blend == 0) ++GpuSpriteScenes;
	if (blend == 0 && Level->HasDynamicLights && gl_light_sprites) ++GpuSpriteLightFrames;
	if (!WorldModeGpuSprites(Level, instances, count, keys, keyCount) || count == 0)
	{
		if (blend == 0) ++GpuSpriteNoPublish;
		return;
	}
	const auto& reached = GpuSpriteVisible[blend];
	if (reached.empty()) return;
	auto colorAdd = [&](const FSpriteAppearanceKey& key)
	{
		return key.Blend == 2 && key.Alpha > 1.f - FLT_EPSILON && gl_usecolorblending && !isFullbrightScene() && key.Fullbright && !key.SoftTexture &&
			!key.Shaded;
	};
	auto styleOf = [&](const FSpriteAppearanceKey& key) -> FRenderStyle
	{
		if (key.Blend == 1) return LegacyRenderStyles[key.Shaded ? STYLE_Shaded : STYLE_Translucent];
		if (key.Blend == 2) return LegacyRenderStyles[key.Shaded ? STYLE_AddShaded : colorAdd(key) ? STYLE_ColorAdd : STYLE_Add];
		return LegacyRenderStyles[STYLE_Normal];
	};
	const uint32_t* frames = nullptr;
	uint32_t frameWords = 0;
	bool rowsReady = false;
	uint64_t frameVersion = 0;
	static std::vector<uint32_t> order, starts;
	auto groupByFrame = [&](const uint32_t* list, size_t size, std::vector<uint32_t>& out)
	{
		uint32_t entries = 0;
		for (size_t i = 0; i < size; ++i) entries = std::max(entries, instances[list[i]].GpuFrame + 1);
		starts.assign(entries + 1, 0);
		for (size_t i = 0; i < size; ++i) ++starts[instances[list[i]].GpuFrame + 1];
		for (uint32_t e = 0; e < entries; ++e) starts[e + 1] += starts[e];
		out.resize(size);
		for (size_t i = 0; i < size; ++i) out[starts[instances[list[i]].GpuFrame]++] = list[i];
	};
	const uint32_t serial = r_sprite_gpu_pertic ? WorldModeGpuSpriteSerial() : 0;
	const std::vector<uint32_t>* drawOrder = &order;
	auto& tic = GpuSpriteTic;
	if (serial != 0)
	{
		if (tic.Serial != serial)
		{
			tic.Serial = serial;
			static std::vector<uint32_t> all[3];
			for (auto& list : all) list.clear();
			for (uint32_t i = 0; i < count; ++i) all[keys[instances[i].Appearance].Blend].push_back(i);
			tic.PosOf.resize(count);
			for (int k = 0; k < 3; ++k)
			{
				groupByFrame(all[k].data(), all[k].size(), tic.Order[k]);
				const auto& ord = tic.Order[k];
				tic.App[k].resize(ord.size());
				for (size_t i = 0; i < ord.size(); ++i)
				{
					tic.App[k][i] = instances[ord[i]].Appearance;
					tic.PosOf[ord[i]] = uint32_t(i);
				}
				tic.Mark[k].assign(ord.size(), 0u);
				tic.Light[k].assign(ord.size() * 3, 0.f);
				tic.LightDone[k].assign(ord.size(), 0);
			}
			tic.MarkSerial = 0;
			tic.GpuLights = false;
			if (r_sprite_gpu_lightpass && Level->HasDynamicLights && gl_light_sprites)
			{
				FGpuLightPack pack;
				if (WorldModeGpuLightPack(Level, pack))
					tic.GpuLights = state.ComputeSpriteLights(reinterpret_cast<const uint32_t*>(instances), count, uint32_t(sizeof(FSpriteInstance) / 4),
						serial, pack);
			}
		}
		if (++tic.MarkSerial == 0)
		{
			for (auto& mark : tic.Mark) std::fill(mark.begin(), mark.end(), 0u);
			tic.MarkSerial = 1;
		}
		for (int32_t index : reached) tic.Mark[blend][tic.PosOf[index]] = tic.MarkSerial;
		drawOrder = &tic.Order[blend];
	}
	else groupByFrame(reinterpret_cast<const uint32_t*>(reached.data()), reached.size(), order);
	const bool lit = Level->HasDynamicLights && gl_light_sprites && !isFullbrightScene();
	const bool gpuLights = lit && serial != 0 && tic.GpuLights && r_sprite_gpu_lightpass;
	if (lit && !gpuLights) GpuSpriteLights.Begin(instances, WorldModeFrameTic(Level));
	static std::vector<uint32_t> passEntries[2];
	static std::vector<uint8_t> keyColorAdd;
	size_t used[2] = {};
	keyColorAdd.assign(keyCount, 0);
	if (blend == 2) for (uint32_t k = 0; k < keyCount; ++k) keyColorAdd[k] = colorAdd(keys[k]);
	{
		const uint64_t lightStarted = I_nsTime();
		for (auto& entries : passEntries) if (entries.size() < drawOrder->size() * 4) entries.resize(drawOrder->size() * 4);
		static const float unlit[3] = {};
		if (serial != 0)
		{
			const auto& ord = tic.Order[blend];
			const auto& app = tic.App[blend];
			const auto& mark = tic.Mark[blend];
			auto& lights = tic.Light[blend];
			auto& done = tic.LightDone[blend];
			for (size_t i = 0; i < ord.size(); ++i)
			{
				if (mark[i] != tic.MarkSerial) continue;
				const int pass = keyColorAdd[app[i]];
				uint32_t* entry = &passEntries[pass][used[pass]];
				used[pass] += 4;
				entry[0] = ord[i];
				if (lit && !gpuLights && !done[i])
				{
					GpuSpriteLights.Get(this, instances[ord[i]], keys, &lights[i * 3]);
					done[i] = 1;
				}
				memcpy(entry + 1, lit && !gpuLights ? &lights[i * 3] : unlit, sizeof(unlit));
			}
		}
		else for (uint32_t index : *drawOrder)
		{
			const FSpriteInstance& in = instances[index];
			const int pass = keyColorAdd[in.Appearance];
			uint32_t* entry = &passEntries[pass][used[pass]];
			used[pass] += 4;
			entry[0] = index;
			float light[3] = {};
			if (lit) GpuSpriteLights.Get(this, in, keys, light);
			memcpy(entry + 1, light, sizeof(light));
		}
		if (lit) GpuSpriteLightNS += I_nsTime() - lightStarted;
	}
	auto drawPass = [&](bool wantColorAdd)
	{
		const uint32_t* visible = passEntries[wantColorAdd].data();
		const uint32_t n = uint32_t(used[wantColorAdd] / 4);
		if (n == 0) return;
		if (!rowsReady)
		{
			WorldModeFillSpriteFrameTable(state, frames, frameWords, frameVersion);
			rowsReady = true;
		}
		const FSpriteFrameEntry* first = WorldModeSpriteFrameEntry(instances[visible[0]].Frame);
		if (first == nullptr) return;
		state.SetMaterial(first->Texture[0], UF_Sprite, CTF_Expand, CLAMP_XY, 0, -1, nullptr);
		state.SetDynLight(0, 0, 0);
		for (uint32_t k = 0; k < keyCount; ++k)
		{
			const FSpriteAppearanceKey& key = keys[k];
			sector_t* sector = key.Sector;
			const FRenderStyle style = styleOf(key);
			FColormap cm;
			if (isFullbrightScene()) cm.Clear();
			else
			{
				cm = sector->Colormap;
				if (key.Fullbright) cm.MakeWhite();
				else if (Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING) cm.Decolorize();
			}
			const int rel = key.Fullbright ? 0 : getExtraLight();
			// [Karto] Full alpha, as ProcessRecord sets ThingColor: Color::White has alpha 0, which a fullbright row would use unmodulated and fail the alpha test.
			PalEntry thingColor = key.Shaded ? PalEntry(key.FillColor) : PalEntry(Color::White);
			thingColor.a = 255;
			// [Karto] A style without texture-mode flags leaves the mode alone; the CPU path starts every run from TM_NORMAL,
			// so a row after a shaded one must not inherit red-is-alpha.
			state.SetTextureMode(TM_NORMAL);
			state.SetTextureMode(style);
			// [Karto] A soft texture's row discards only zero alpha, as DrawSprite's no-alpha-test case shows under source-alpha
			// blending; every row keeps the alpha-test shader variant (the variant without it drew opaque black quads).
			if (key.Blend != 0)
			{
				if (key.SoftTexture) state.AlphaFunc(Alpha_Greater, 0.f);
				else state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
			}
			state.SetObjectColor(key.Fullbright ? thingColor : thingColor.Modulate(sector->SpecialColors[sector_t::sprites]));
			state.SetAddColor(sector->AdditiveColors[sector_t::sprites] | 0xff000000);
			SetColor(state, Level, lightmode, key.LightLevel, rel, isFullbrightScene(), cm, key.Alpha);
			uint8_t foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);
			if (cm.FadeColor.isBlack()) foglevel = key.LightLevel;
			const bool additiveFog = style.BlendOp == STYLEOP_Add && style.DestAlpha == STYLEALPHA_One;
			SetFog(state, Level, lightmode, foglevel, rel, isFullbrightScene(), &cm, additiveFog);
			state.AddSpriteRow();
		}
		const FRenderStyle drawStyle = blend == 0 ? LegacyRenderStyles[STYLE_Source] :
			blend == 1 ? LegacyRenderStyles[STYLE_Translucent] : LegacyRenderStyles[wantColorAdd ? STYLE_ColorAdd : STYLE_Add];
		state.SetRenderStyle(drawStyle);
		state.SetTextureMode(blend == 0 ? LegacyRenderStyles[STYLE_Normal] : drawStyle);
		const auto& vp = Viewpoint;
		const FSpriteViewParams view = SpriteViewParams(this);
		const float params[32] = {
			float(vp.Pos.X), float(vp.Pos.Y), float(vp.Pos.Z), float(vp.TicFrac),
			float(vp.ViewVector.X), float(vp.ViewVector.Y), float(Net_ModifyFrac(false, vp.TicFrac)), float(WorldModeFrameTic(Level)),
			vp.bDoOrtho ? 1.f : 0.f, float(vp.Angles.Yaw.Degrees()), float(gl_spriteclip), float(gl_sclipthreshold),
			float(gl_sclipfactor), r_sprite_indexed ? 1.f : 0.f, view.PixelStretch, view.AxisDeg,
			view.Side[0].X, view.Side[0].Y, view.Side[0].Z, view.PitchDeg,
			view.Up[0].X, view.Up[0].Y, view.Up[0].Z, gpuLights ? 1.f : 0.f,
			view.Side[1].X, view.Side[1].Y, view.Side[1].Z, clamp(float(r_spriteclipanamorphicminbias), 0.3f, 1.0f),
			view.Up[1].X, view.Up[1].Y, view.Up[1].Z, r_debug_nolimitanamorphoses ? 1.f : 0.f };
		if (state.DrawSpriteInstances(reinterpret_cast<const uint32_t*>(instances), count, uint32_t(sizeof(FSpriteInstance) / 4), serial,
			visible, n, frames, frameWords, frameVersion, params, 8))
		{
			++GpuSpriteDraws;
			GpuSpriteInstances += n;
			GpuSpriteRows += keyCount;
		}
		else ++GpuSpriteDeclined;
	};
	drawPass(false);
	if (blend == 2) drawPass(true);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	if (blend != 0)
	{
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		state.EnableTexture(true);
	}
}

CCMD(sprite_gpu_check)
{
	Printf("SPRITEGPUCHECK checked=%llu texture_mismatch=%llu rotation_mismatch=%llu geometry_mismatch=%llu max_error=%.5f\n",
		(unsigned long long)SpriteGpuChecked, (unsigned long long)SpriteGpuTextureMismatch, (unsigned long long)SpriteGpuRotationMismatch,
		(unsigned long long)SpriteGpuGeometryMismatch, SpriteGpuMaxError);
}

//==========================================================================
//
// [Karto] Sprite record path: expressions keep Process's operand order and types so the result is bit-identical.
//
//==========================================================================

void HWSprite::ProcessRecord(HWDrawInfo *di, const FSpriteRecord& r, sector_t * sector, area_t in_area, int thruportal)
{
	const auto &vp = di->Viewpoint;
	sector_t rs;
	sector_t * rendersector;

	const double alpha = (r.renderflags2 & RF2_INTERPOLATEALPHA) ? r.PrevAlpha * (1.0 - vp.TicFrac) + r.Alpha * vp.TicFrac : r.Alpha;
	if (!r.RenderStyle.IsVisible(alpha))
	{
		if (!(r.flags & MF_STEALTH) || !di->isStealthVision() || r.Live == WorldModeLiveActor(vp.camera)) return;
	}
	if ((!r_debug_disable_vis_filter && !!(r.RenderRequired & ~r_renderercaps)) ||
		(!!(r.RenderHidden & r_renderercaps)))
		return;

	int spritenum = r.sprite;
	DVector2 sprscale = (r.renderflags2 & RF2_INTERPOLATESCALE) ? r.PrevScale * (1.0 - vp.TicFrac) + r.Scale * vp.TicFrac : r.Scale;
	DVector3 thingpos = (r.renderflags & RF_DONTINTERPOLATE) ? r.Pos : r.Prev * (1.0 - vp.TicFrac) + r.Pos * vp.TicFrac;
	if (thruportal == 1) thingpos += di->Level->Displacements.getOffset(r.PortalGroup, sector->PortalGroup);

	modelframe = nullptr;
	modelframeflags = 0;

	if (fabs(thingpos.X - vp.Pos.X) < 2 && fabs(thingpos.Y - vp.Pos.Y) < 2
		&& vp.Pos.Z >= thingpos.Z - 2 && vp.Pos.Z <= thingpos.Z + r.Height + 2
		&& r.Moving)
	{
		return;
	}

	if (thruportal != 2 && di->mClipPortal != nullptr)
	{
		int clipres = di->mClipPortal->ClipPoint(thingpos.XY());
		if (clipres == PClip_InFront) return;
	}
	Angles = r.Angles;

	if (sector->sectornum != r.Sector->sectornum && !thruportal)
	{
		rendersector = hw_FakeFlat(r.Sector, in_area, false, &rs);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	x = thingpos.X + r.WorldOffset.X;
	z = thingpos.Z + r.WorldOffset.Z;
	y = thingpos.Y + r.WorldOffset.Y;
	z -= r.Floorclip;
	{
		const double ticfrac = Net_ModifyFrac(r.ClientSide, vp.TicFrac);
		float fz = (r.flags2 & MF2_FLOATBOB) ? BobSin(r.FloatBobPhase + WorldModeFrameTic(di->Level) * r.FloatBobFactor + ticfrac) * r.FloatBobStrength : 0;
		z += fz;
	}

	bool mirror = false;
	DAngle ang = (thingpos - vp.Pos).Angle();
	if (di->Viewpoint.bDoOrtho) ang = vp.Angles.Yaw;
	DAngle sprangle = (r.flags7 & MF7_SPRITEANGLE) ? r.SpriteAngle : ang - (r.Angles.Yaw + r.SpriteRotation);
	FTextureID patch = sprites[spritenum].GetSpriteFrame(r.frame, -1, sprangle, &mirror, !!(r.renderflags & RF_SPRITEFLIP));
	if (!patch.isValid()) return;
	auto tex = TexMan.GetGameTexture(patch, false);
	if (!tex || !tex->isValid()) return;
	auto& spi = tex->GetSpritePositioning(true);

	offx = (float)(r.renderflags2 & RF2_FLIPSPRITEOFFSETX ? r.SpriteOffset.X : -r.SpriteOffset.X);
	offy = (float)(r.renderflags2 & RF2_FLIPSPRITEOFFSETY ? r.SpriteOffset.Y : -r.SpriteOffset.Y);

	vt = spi.GetSpriteVT();
	vb = spi.GetSpriteVB();
	if (r.renderflags & RF_YFLIP) std::swap(vt, vb);

	auto rect = spi.GetSpriteRect();
	// [Karto] Process toggles the actor's RF_XFLIP around this test for RF_SPRITEFLIP; the record path uses a local copy.
	ActorRenderFlags rflags = r.renderflags;
	if (rflags & RF_SPRITEFLIP) rflags ^= RF_XFLIP;
	if (mirror ^ !!(rflags & RF_XFLIP))
	{
		rect.left = -rect.width - rect.left;
		ul = spi.GetSpriteUL();
		ur = spi.GetSpriteUR();
	}
	else
	{
		ul = spi.GetSpriteUR();
		ur = spi.GetSpriteUL();
	}

	texture = tex;
	if (texture->isHardwareCanvas()) std::swap(vt, vb);

	const double isoscaleY = 1.0;
	rect.Scale(sprscale.X, sprscale.Y * isoscaleY);
	if ((r.renderflags & RF_ROLLSPRITE) && !(r.renderflags2 & RF2_STRETCHPIXELS))
	{
		double ps = di->Level->pixelstretch;
		double mult = 1.0 / sqrt(ps);
		rect.Scale(mult * ps, mult);
	}

	float rightfac = -rect.left;
	float leftfac = rightfac - rect.width;
	z1 = z - rect.top;
	z2 = z1 - rect.height;

	float spriteheight = sprscale.Y * rect.height * isoscaleY;
	if (spriteheight > 0 && gl_spriteclip > 0)
	{
		PerformSpriteClipAdjustment(FSpriteClipSource{ r.Sector, r.Pos.XY(), r.floorz, r.ceilingz, r.Floorclip, r.flags, r.flags2, r.flags3,
			false, r.IsInventory }, thingpos.XY(), spriteheight);
	}

	{
		float viewvecX = vp.ViewVector.X;
		float viewvecY = vp.ViewVector.Y;

		x1 = x - viewvecY*leftfac;
		x2 = x - viewvecY*rightfac;
		y1 = y + viewvecX*leftfac;
		y2 = y + viewvecX*rightfac;
	}

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

	if (gl_spriteclip == -1)
	{
		const float vpx = vp.Pos.X, vpy = vp.Pos.Y, vpz = vp.Pos.Z;
		const float spbias = SpriteAnamorphicBias(z1, z2, r.Sector->floorplane.ZatPoint(r.Pos) - r.Floorclip,
			r.Sector->ceilingplane.ZatPoint(thingpos), r.Sector->floorplane.ZatPoint(vp.Pos), r.Sector->ceilingplane.ZatPoint(vp.Pos),
			vpx, vpy, vpz, thingpos.X, thingpos.Y, r.radius, r.waterlevel >= 1 && r.waterlevel <= 2);
		const float vpbias = 1.0 - spbias;
		x1 = x1 * spbias + vpx * vpbias;
		y1 = y1 * spbias + vpy * vpbias;
		z1 = z1 * spbias + vpz * vpbias;
		x2 = x2 * spbias + vpx * vpbias;
		y2 = y2 * spbias + vpy * vpbias;
		z2 = z2 * spbias + vpz * vpbias;
	}

	bool enhancedvision = false;

	fullbright = (r.flags5 & MF5_BRIGHT) ||
		((r.renderflags & RF_FULLBRIGHT) && (!texture || !texture->isFullbrightDisabled()));

	if (fullbright) lightlevel = 255;
	else
	{
		int level = rendersector->GetSpriteLight();
		if (r.flags8 & MF8_ADDLIGHTLEVEL) level += r.LightLevel;
		else if (r.LightLevel > -1) level = r.LightLevel;
		lightlevel = RescaleLightLevel(level);
	}

	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255);

	lightlevel = rendersector->CheckSpriteGlow(lightlevel, thingpos);

	ThingColor = (r.RenderStyle.Flags & STYLEF_ColorIsFixed) ? r.fillcolor : Color::White;
	ThingColor.a = 255;
	RenderStyle = r.RenderStyle;

	if (di->isFullbrightScene())
	{
		enhancedvision = di->isStealthVision();

		Colormap.Clear();

		if (di->isNightvision())
		{
			if ((r.IsInventory || r.flags3&MF3_ISMONSTER || r.flags&MF_MISSILE || r.flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &di->Level->sectors[rendersector->sectornum] || in_area != area_below)
			{
				Colormap.MakeWhite();
			}
			else
			{
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = r.Translation;

	OverrideShader = -1;
	trans = alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency && (r.renderflags & RF_ZDOOMTRANS))
	{
		if (r_UseVanillaTransparency == 1 || AutoTrans.CheckKey(r.Class->TypeName) != nullptr)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		trans = 1.f;

		if (!gl_sprite_blend ||
			(hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && !(r.renderflags2 & RF2_BILLBOARDNOFACECAMERA)) ||
			r.renderflags2 & RF2_BILLBOARDFACECAMERA))
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((texture && texture->GetTranslucency()) || (RenderStyle.Flags & STYLEF_RedIsAlpha))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			trans = 0.5f;
			FRenderStyle rstyle = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rstyle.Flags;
		}
		else if (r.flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	if (trans == 0.0f) return;

	actor = nullptr;
	record = &r;
	index = r.SpawnOrder;

	if (r_sprite_gpu_check && r.Instance >= 0)
	{
		const FSpriteInstance* in = WorldModeSpriteInstance(r.Instance);
		FSpriteInstanceGeometry g;
		if (in != nullptr && SpriteInstanceGeometry(di, *in, g))
		{
			++SpriteGpuChecked;
			// [Karto] A texture difference with equal mirroring is a rotation picked on the other side of a boundary
			// (single against double precision); anything else is a real mismatch.
			if (g.Texture != texture)
			{
				if (g.ul == ul && g.ur == ur) ++SpriteGpuRotationMismatch;
				else ++SpriteGpuTextureMismatch;
			}
			else
			{
				float err = std::max({ fabsf(g.x1 - x1), fabsf(g.y1 - y1), fabsf(g.z1 - z1), fabsf(g.x2 - x2), fabsf(g.y2 - y2),
					fabsf(g.z2 - z2), fabsf(g.depth - depth) });
				FVector3 v[4];
				CalculateVertices(di, v, &di->Viewpoint.Pos);
				for (int i = 0; i < 4; ++i)
					err = std::max({ err, fabsf(g.v[i].X - v[i].X), fabsf(g.v[i].Y - v[i].Y), fabsf(g.v[i].Z - v[i].Z) });
				SpriteGpuMaxError = std::max(SpriteGpuMaxError, double(err));
				if (err > 0.05f || g.ul != ul || g.ur != ur || g.vt != vt || g.vb != vb) ++SpriteGpuGeometryMismatch;
			}
		}
	}
	particle = nullptr;
	lightlist = nullptr;

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}



//==========================================================================
//
//
//
//==========================================================================

void HWSprite::ProcessParticle(HWDrawInfo *di, particle_t *particle, sector_t *sector, DVisualThinker *spr)//, int shade, int fakeside)
{
	if (!particle || particle->alpha <= 0)
		return;

	if (spr && !spr->ValidTexture())
		return;

	lightlevel = RescaleLightLevel(spr ? spr->GetLightLevel(sector) : sector->GetSpriteLight());
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);

	trans = particle->alpha;
	OverrideShader = (particle->flags & SPF_ALLOWSHADERS) ? -1 : 0;
	modelframe = nullptr;
	texture = nullptr;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;
	index = 0;
	actor = nullptr;
	this->particle = particle;
	fullbright = particle->flags & SPF_FULLBRIGHT;

	if (di->isFullbrightScene())
	{
		Colormap.Clear();
	}
	else if (!(particle->flags & SPF_FULLBRIGHT))
	{
		TArray<lightlist_t> & lightlist=sector->e->XFloor.lightlist;
		double lightbottom;

		Colormap = sector->Colormap;
		for(unsigned int i=0;i<lightlist.Size();i++)
		{
			if (i<lightlist.Size()-1) lightbottom = lightlist[i+1].plane.ZatPoint(particle->Pos);
			else lightbottom = sector->floorplane.ZatPoint(particle->Pos);

			if (lightbottom < particle->Pos.Z)
			{
				lightlevel = RescaleLightLevel(*lightlist[i].p_lightlevel);
				Colormap.CopyLight(lightlist[i].extra_colormap);
				break;
			}
		}
		if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();	// ZDoom never applies colored light to particles.
		}
	}
	else
	{
		lightlevel = 255;
		Colormap = sector->Colormap;
		Colormap.ClearColor();
	}

	if(particle->style != STYLE_None)
	{
		RenderStyle = particle->style;
	}
	else
	{
		RenderStyle = STYLE_Translucent;
	}

	ThingColor = particle->color;
	ThingColor.a = 255;
	const auto& vp = di->Viewpoint;

	double timefrac = Net_ModifyParticleFrac(particle, vp.TicFrac);
	if (paused || (di->Level->isFrozen() && !(particle->flags & SPF_NOTIMEFREEZE)))
		timefrac = 0.;


	if (spr && !(spr->flags & VTF_IsParticle))
	{
		AdjustVisualThinker(di, spr, sector);
	}
	else
	{
		bool has_texture = false;
		bool custom_animated_texture = false;
		int particle_style = 0;
		float size = particle->size;
		if (!spr)
		{
			has_texture = particle->texture.isValid();
			custom_animated_texture = (particle->flags & SPF_LOCAL_ANIM) && particle->animData.ok;
			particle_style = has_texture ? 2 : gl_particles_style; // Treat custom texture the same as smooth particles
		}
		else
		{
			size = float(spr->Scale.X);
			const int ptype = spr->GetParticleType();
			particle_style = (ptype != PT_DEFAULT) ? ptype : gl_particles_style;
		}
		// [BB] Load the texture for round or smooth particles
		if (particle_style)
		{
			FTextureID lump;
			if (particle_style == 1)
			{
				lump = TexMan.glPart2;
			}
			else if (particle_style == 2)
			{
				if(custom_animated_texture)
				{
					lump = TexAnim.UpdateStandaloneAnimation(particle->animData, WorldModeFrameTimer(di->Level) + timefrac);
				}
				else if(has_texture)
				{
					lump = particle->texture;
				}
				else
				{
					lump = TexMan.glPart;
				}
			}
			else
			{
				lump.SetNull();
			}

			if (lump.isValid())
			{
				translation = NO_TRANSLATION;

				ul = vt = 0;
				ur = vb = 1;

				texture = TexMan.GetGameTexture(lump, !custom_animated_texture);
			}
		}


		float xvf = (particle->Vel.X) * timefrac;
		float yvf = (particle->Vel.Y) * timefrac;
		float zvf = (particle->Vel.Z) * timefrac;

		offx = 0.f;
		offy = 0.f;

		x = float(particle->Pos.X) + xvf;
		y = float(particle->Pos.Y) + yvf;
		z = float(particle->Pos.Z) + zvf;

		if(particle->flags & SPF_ROLL)
		{
			float rvf = (particle->RollVel) * timefrac;
			Angles.Roll = TAngle<double>::fromDeg(particle->Roll + rvf);
		}

		float factor;
		if (particle_style == 1) factor = 1.3f / 7.f;
		else if (particle_style == 2) factor = 2.5f / 7.f;
		else factor = 1 / 7.f;
		float scalefac= size * factor;

		float ps = di->Level->pixelstretch;

		scalefac /= sqrt(ps); // shrink it slightly to account for the stretch

		float viewvecX = vp.ViewVector.X * scalefac * ps;
		float viewvecY = vp.ViewVector.Y * scalefac;

		x1=x+viewvecY;
		x2=x-viewvecY;
		y1=y-viewvecX;
		y2=y+viewvecX;
		z1=z-scalefac;
		z2=z+scalefac;

		depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

		// [BB] Translucent particles have to be rendered without the alpha test.
		if (particle_style != 2 && trans>=1.0f-FLT_EPSILON) hw_styleflags = STYLEHW_Solid;
		else hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright)
		lightlist = &sector->e->XFloor.lightlist;
	else
		lightlist = nullptr;

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

// [MC] VisualThinkers are to be rendered akin to actor sprites. The reason this whole system
// is hitching a ride on particle_t is because of the large number of checks with
// HWSprite elsewhere in the draw lists.
void HWSprite::AdjustVisualThinker(HWDrawInfo* di, DVisualThinker* spr, sector_t* sector)
{
	translation = spr->Translation;

	const auto& vp = di->Viewpoint;
	double timefrac = Net_ModifyObjectFrac(spr, vp.TicFrac);

	if (paused || spr->isFrozen())
		timefrac = 0.;

	bool custom_anim = ((spr->PT.flags & SPF_LOCAL_ANIM) && spr->PT.animData.ok);

	texture = TexMan.GetGameTexture(
			custom_anim
			? TexAnim.UpdateStandaloneAnimation(spr->PT.animData, WorldModeFrameTimer(di->Level) + timefrac)
			: spr->PT.texture, !custom_anim);

	if (spr->flags & VTF_DontInterpolate)
		timefrac = 0.;

	FVector3 interp = spr->InterpolatedPosition(timefrac);
	x = interp.X;
	y = interp.Y;
	z = interp.Z;

	offx = (float)spr->GetOffset(false);
	offy = (float)spr->GetOffset(true);

	if (spr->PT.flags & SPF_ROLL)
		Angles.Roll = TAngle<double>::fromDeg(spr->InterpolatedRoll(timefrac));

	auto& spi = texture->GetSpritePositioning(0);

	vt = spi.GetSpriteVT();
	vb = spi.GetSpriteVB();
	ul = spi.GetSpriteUR();
	ur = spi.GetSpriteUL();

	auto r = spi.GetSpriteRect();
	r.Scale(spr->Scale.X, spr->Scale.Y);

	if ((spr->PT.flags & SPF_ROLL) && !(spr->PT.flags & SPF_STRETCHPIXELS))
	{
		double ps = di->Level->pixelstretch;
		double mult = 1.0 / sqrt(ps); // shrink slightly
		r.Scale(mult * ps, mult);
	}
	if (spr->flags & VTF_FlipX)
	{
		std::swap(ul,ur);
		r.left = -r.width - r.left;	// mirror the sprite's x-offset
	}
	if (spr->flags & VTF_FlipY)	std::swap(vt,vb);

	float viewvecX = vp.ViewVector.X;
	float viewvecY = vp.ViewVector.Y;
	float rightfac = -r.left;
	float leftfac = rightfac - r.width;

	x1 = x - viewvecY * leftfac;
	x2 = x - viewvecY * rightfac;
	y1 = y + viewvecX * leftfac;
	y2 = y + viewvecX * rightfac;
	z1 = z - r.top;
	z2 = z1 - r.height;

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

	// [BB] Translucent particles have to be rendered without the alpha test.
	hw_styleflags = STYLEHW_NoAlphaTest;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ProcessActorsInPortal(FLinePortalSpan *glport, area_t in_area)
{
	TMap<AActor*, bool> processcheck;
	if (glport->validcount == rendervalidcount) return;	// only process once per frame
	glport->validcount = rendervalidcount;
	const auto &vp = Viewpoint;
	for (auto port : glport->lines)
	{
		line_t *line = port->mOrigin;
		if (line->isLinePortal())	// only crossable ones
		{
			FLinePortal *port2 = port->mDestination->getPortal();
			// process only if the other side links back to this one.
			if (port2 != nullptr && port->mDestination == port2->mOrigin && port->mOrigin == port2->mDestination)
			{
				for (portnode_t *node = port->lineportal_thinglist; node != nullptr; node = node->m_snext)
				{
					AActor *th = node->m_thing;

					// process each actor only once per portal.
					bool *check = processcheck.CheckKey(th);
					if (check && *check) continue;
					processcheck[th] = true;

					DAngle savedangle = th->Angles.Yaw;
					DVector3 savedpos = th->Pos();
					DVector3 newpos = savedpos;
					sector_t fakesector;

					if (!vp.showviewer)
					{
						AActor *viewmaster = th;
						if ((th->flags8 & MF8_MASTERNOSEE) && th->master != nullptr)
						{
							viewmaster = th->master;
						}

						if (viewmaster == vp.camera)
						{
							DVector3 vieworigin = viewmaster->Pos();

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the original position
								continue;
							}

							P_TranslatePortalXY(line, vieworigin.X, vieworigin.Y);
							P_TranslatePortalZ(line, vieworigin.Z);

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the translated position
								// (This is required for MASTERNOSEE actors with 3D models)
								continue;
							}
						}
					}

					P_TranslatePortalXY(line, newpos.X, newpos.Y);
					P_TranslatePortalZ(line, newpos.Z);
					P_TranslatePortalAngle(line, th->Angles.Yaw);
					th->SetXYZ(newpos);
					th->Prev += newpos - savedpos;

					HWSprite spr;

					// [Nash] draw sprite shadow
					if (R_ShouldDrawSpriteShadow(th))
					{
						spr.Process(this, th, hw_FakeFlat(th->Sector, in_area, false, &fakesector), in_area, 2, true);
					}

					// This is called from the worker thread and must not alter the fake sector cache.
					spr.Process(this, th, hw_FakeFlat(th->Sector, in_area, false, &fakesector), in_area, 2);
					th->Angles.Yaw = savedangle;
					th->SetXYZ(savedpos);
					th->Prev -= newpos - savedpos;
				}
			}
		}
	}
}
