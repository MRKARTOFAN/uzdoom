/*
** exposurefused.fp
**
** [Karto] Up to three steps of exposureaverage.fp in one pass (r_pp_fused), rebuilding its textureGather with texelFetch
** in the same order and in 32-bit float, so the result matches the chain of single steps.
**
**---------------------------------------------------------------------------
**
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D ExposureTexture;

ivec2 Half(ivec2 size)
{
	return max(size / 2, ivec2(1));
}

float Level0(ivec2 t)
{
	vec4 v = texelFetch(ExposureTexture, t, 0);
	return Channel == 3 ? v.w : v.x;
}

void Footprint(ivec2 j, ivec2 size, ivec2 parent, out ivec2 i0, out ivec2 i1)
{
	vec2 c = (vec2(j) + 0.5) / vec2(size) * vec2(parent) - 0.5;
	ivec2 base = ivec2(floor(c));
	i0 = clamp(base, ivec2(0), parent - ivec2(1));
	i1 = clamp(base + ivec2(1), ivec2(0), parent - ivec2(1));
}

float Level1(ivec2 j, ivec2 s0)
{
	ivec2 i0, i1;
	Footprint(j, Half(s0), s0, i0, i1);
	return (Level0(ivec2(i0.x, i1.y)) + Level0(i1) + Level0(ivec2(i1.x, i0.y)) + Level0(i0)) * 0.25;
}

float Level2(ivec2 j, ivec2 s0)
{
	ivec2 s1 = Half(s0);
	ivec2 i0, i1;
	Footprint(j, Half(s1), s1, i0, i1);
	return (Level1(ivec2(i0.x, i1.y), s0) + Level1(i1, s0) + Level1(ivec2(i1.x, i0.y), s0) + Level1(i0, s0)) * 0.25;
}

float Level3(ivec2 j, ivec2 s0)
{
	ivec2 s2 = Half(Half(s0));
	ivec2 i0, i1;
	Footprint(j, Half(s2), s2, i0, i1);
	return (Level2(ivec2(i0.x, i1.y), s0) + Level2(i1, s0) + Level2(ivec2(i1.x, i0.y), s0) + Level2(i0, s0)) * 0.25;
}

void main()
{
	ivec2 s0 = textureSize(ExposureTexture, 0);
	ivec2 target = Depth == 1 ? Half(s0) : Depth == 2 ? Half(Half(s0)) : Half(Half(Half(s0)));
	ivec2 p = min(ivec2(TexCoord * vec2(target)), target - ivec2(1));
	float value = Depth == 1 ? Level1(p, s0) : Depth == 2 ? Level2(p, s0) : Level3(p, s0);
	FragColor = vec4(value, 0.0, 0.0, 1.0);
}
