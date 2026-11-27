/*
** hw_lighting.h
**
** Light level / fog management / dynamic lights
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "c_cvars.h"
#include "v_palette.h"

#include "r_utility.h"

struct Colormap;

EXTERN_CVAR(Int, r_extralight)

inline int hw_ClampLight(int lightlevel)
{
	return clamp(lightlevel, 0, 255);
}

template<bool doClamp = true>
inline int RescaleLightLevel(int lightlevel) // TODO/tidy: move out of hwrenderer namespace
{
	// [Karto] Fast-math inlining rounded the same sector differently in native walls and GPU source collection;
	// integer scaling keeps truncation and exact identity at zero extra light.
	const int level = doClamp ? clamp(lightlevel, 0, 255) : max(lightlevel, 0);
	const int extra = r_extralight;
	return max(int(int64_t(level) * (255 - extra) / 255) + extra, 0);
}

inline int CalcRelLight(int lightlevel, int orglightlevel, int rel)
{
	if (orglightlevel >= 253)			// with the software renderer fake contrast won't be visible above this.
	{
		return 0;
	}
	else if (lightlevel - rel > 256)	// the brighter part of fake contrast will be clamped so also clamp the darker part by the same amount for better looks
	{
		return 256 - lightlevel + rel;
	}
	else
	{
		return rel;
	}
}

EXTERN_CVAR(Int, gl_weaponlight);

inline	int getExtraLight()
{
	return r_viewpoint.extralight * gl_weaponlight;
}
